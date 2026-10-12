//
// Copyright 2026 AUTOMATOUS.IO
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

// shelly_settings.cpp

#include "shelly_settings.h"

#include <pthread.h>
#include <string.h>

#include <esp_log.h>
#include <esp_timer.h>
#include <nvs.h>
#include <sdkconfig.h>

#include <relay.h>

#include "shelly_app.h"
#include "shelly_calibration.h"
#include "shelly_setup_code.h"
#include "shelly_wifi_password.h"

static const char *TAG = "settings";

static const char *NVS_NAMESPACE = "shelly_cfg";
// One key per wall switch input; SW1 keeps the single-input key.
static const char *const NVS_KEY_SWITCH_MODE[] = {"sw_mode", "sw_mode2"};
_Static_assert(RELAY_CHANNELS <= 2, "one switch mode key per channel");
static const char *NVS_KEY_PROFILE = "profile";
// SW1's mode while it was an outlet, restored when it goes back to one.
static const char *NVS_KEY_OUTLET_SWITCH_MODE = "sw_mode_out";

// The wall switch mode the page recommends for a garage door opener, and
// sets when the device type changes to one: a wall button that springs
// back triggers the door once per press.
#define OPENER_SWITCH_MODE UI_SWITCH_MODE_PUSH

// Guards the switch mode and the waiting change (the maintenance page and
// the button run on their own tasks). Taken before the setup code's own
// lock, never after it.
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

// Read by the switch task on every change: plain aligned words.
static volatile ui_switch_mode_t s_switch_mode[RELAY_CHANNELS];
static volatile bool s_loaded = false;

// The device type this boot runs as, and the saved one (they differ once a
// change is saved, until the next boot).
static device_profile_t s_profile = DEVICE_PROFILE_OUTLET;
static device_profile_t s_profile_saved = DEVICE_PROFILE_OUTLET;
static bool s_profile_loaded = false;

static shelly_change_t s_change;
static int64_t s_change_at_us = 0;
static uint32_t s_saved_count = 0;

void shelly_settings_init(void)
{
    for (int ch = 0; ch < RELAY_CHANNELS; ch++) {
        ui_switch_mode_t mode = UI_SWITCH_MODE_TOGGLE;
#if CONFIG_SHELLY_MAINTENANCE
        // Without Wi-Fi mode there is no page to change a saved mode back:
        // such a build always uses Toggle.
        nvs_handle_t h;
        if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
            uint8_t v;
            if (nvs_get_u8(h, NVS_KEY_SWITCH_MODE[ch], &v) == ESP_OK) {
                if (v < UI_SWITCH_MODE_COUNT) {
                    mode = (ui_switch_mode_t)v;
                } else {
                    ESP_LOGW(TAG, "Unknown saved switch mode %u: using toggle", v);
                }
            }
            nvs_close(h);
        }
#endif
        s_switch_mode[ch] = mode;
#if RELAY_CHANNELS > 1
        ESP_LOGI(TAG, "Wall switch %d mode: %s", ch + 1, ui_switch_mode_name(mode));
#else
        ESP_LOGI(TAG, "Wall switch mode: %s", ui_switch_mode_name(mode));
#endif
    }
    s_loaded = true;
}

ui_switch_mode_t shelly_switch_mode_get_ch(int ch)
{
    // A switch change can arrive before shelly_ui_init(): load it then.
    if (!s_loaded) {
        shelly_settings_init();
    }
    return ch >= 0 && ch < RELAY_CHANNELS ? s_switch_mode[ch] : UI_SWITCH_MODE_TOGGLE;
}

ui_switch_mode_t shelly_switch_mode_get(void)
{
    return shelly_switch_mode_get_ch(0);
}

bool shelly_profile_available(void)
{
#if CONFIG_SHELLY_HAS_OPENER_MODE && CONFIG_SHELLY_MAINTENANCE
    return true;
#else
    return false;
#endif
}

// With s_lock held.
static void load_profile(void)
{
    if (s_profile_loaded) {
        return;
    }
    s_profile_loaded = true;
    // Without Wi-Fi mode there is no page to change a saved type back: such
    // a build is always an outlet, like its switch mode is always Toggle.
    if (!shelly_profile_available()) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, NVS_KEY_PROFILE, &v) == ESP_OK) {
            if (v < DEVICE_PROFILE_COUNT) {
                s_profile = (device_profile_t)v;
            } else {
                ESP_LOGW(TAG, "Unknown saved device type %u: using outlet", v);
            }
        }
        nvs_close(h);
    }
    s_profile_saved = s_profile;
    ESP_LOGI(TAG, "Device type: %s", device_profile_name(s_profile));
}

device_profile_t shelly_profile_get(void)
{
    pthread_mutex_lock(&s_lock);
    load_profile();
    device_profile_t p = s_profile;
    pthread_mutex_unlock(&s_lock);
    return p;
}

device_profile_t shelly_profile_saved(void)
{
    pthread_mutex_lock(&s_lock);
    load_profile();
    device_profile_t p = s_profile_saved;
    pthread_mutex_unlock(&s_lock);
    return p;
}

// One setting in NVS_NAMESPACE; what names it in the log.
static bool save_u8(const char *key, uint8_t value, const char *what)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, key, value);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Saving the %s failed: %s", what, esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool save_profile(device_profile_t profile)
{
    return save_u8(NVS_KEY_PROFILE, (uint8_t)profile, "device type");
}

static bool save_switch_mode(int ch, ui_switch_mode_t mode)
{
    return save_u8(NVS_KEY_SWITCH_MODE[ch], (uint8_t)mode, "switch mode");
}

// With s_lock held, after a device type change from `from` to `to` is
// saved: give SW1 the mode that suits the new type. Outlet to opener keeps
// the outlet's mode aside and sets OPENER_SWITCH_MODE; opener to outlet
// puts the kept mode back. A failure here leaves the mode as it was.
static void switch_mode_follow_profile(device_profile_t from, device_profile_t to)
{
    if (from == to || shelly_ui_switch_role() != UI_SWITCH_TOGGLE) {
        return;
    }
    ui_switch_mode_t mode;
    if (to == DEVICE_PROFILE_OPENER) {
        if (!save_u8(NVS_KEY_OUTLET_SWITCH_MODE, (uint8_t)s_switch_mode[0], "outlet's switch mode")) {
            return;
        }
        mode = OPENER_SWITCH_MODE;
    } else {
        mode = UI_SWITCH_MODE_TOGGLE;
        nvs_handle_t h;
        if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
            uint8_t v;
            if (nvs_get_u8(h, NVS_KEY_OUTLET_SWITCH_MODE, &v) == ESP_OK && v < UI_SWITCH_MODE_COUNT) {
                mode = (ui_switch_mode_t)v;
            }
            nvs_close(h);
        }
    }
    if (mode != s_switch_mode[0] && save_switch_mode(0, mode)) {
        s_switch_mode[0] = mode;
        ESP_LOGW(TAG, "Wall switch mode set to %s for the new device type", ui_switch_mode_name(mode));
    }
}

// With s_lock held: nothing waits any more, and no password stays behind.
static void change_clear(void)
{
    s_change.kind = SHELLY_CHANGE_NONE;
    memset(s_change.wifi_password, 0, sizeof(s_change.wifi_password));
}

// With s_lock held.
static bool change_live(void)
{
    if (s_change.kind == SHELLY_CHANGE_NONE) {
        return false;
    }
    if (esp_timer_get_time() - s_change_at_us >= (int64_t)SHELLY_CHANGE_CONFIRM_S * 1000000) {
        change_clear();
        return false;
    }
    return true;
}

void shelly_change_stage(const shelly_change_t *change)
{
    pthread_mutex_lock(&s_lock);
    s_change = *change;
    s_change_at_us = esp_timer_get_time();
    pthread_mutex_unlock(&s_lock);
}

bool shelly_switch_mode_stage_ch(int ch, ui_switch_mode_t mode)
{
    if (shelly_ui_switch_role() != UI_SWITCH_TOGGLE || (unsigned)mode >= UI_SWITCH_MODE_COUNT || ch < 0 ||
        ch >= RELAY_CHANNELS) {
        return false;
    }
    shelly_change_t change = {};
    change.kind = SHELLY_CHANGE_SWITCH_MODE;
    change.switch_mode = mode;
    change.channel = (uint8_t)ch;
    shelly_change_stage(&change);
    return true;
}

bool shelly_switch_mode_stage(ui_switch_mode_t mode)
{
    return shelly_switch_mode_stage_ch(0, mode);
}

bool shelly_cal_stage(int ch, const cal_ratios_t *ratios)
{
    if (!shelly_cal_available() || !cal_ratios_valid(ratios) || ch < 0 || ch >= RELAY_CHANNELS) {
        return false;
    }
    shelly_change_t change = {};
    change.kind = SHELLY_CHANGE_CALIBRATION;
    change.channel = (uint8_t)ch;
    change.cal = *ratios;
    shelly_change_stage(&change);
    return true;
}

bool shelly_wifi_password_stage(const char *password)
{
    if (password[0] != '\0' && !wifi_password_valid(password)) {
        return false;
    }
    shelly_change_t change = {};
    change.kind = SHELLY_CHANGE_WIFI_PASSWORD;
    strcpy(change.wifi_password, password);
    shelly_change_stage(&change);
    // explicit_bzero: the compiler drops a memset of a local about to die.
    explicit_bzero(change.wifi_password, sizeof(change.wifi_password));
    return true;
}

bool shelly_profile_stage(device_profile_t profile)
{
    if (!shelly_profile_available() || (unsigned)profile >= DEVICE_PROFILE_COUNT) {
        return false;
    }
    shelly_change_t change = {};
    change.kind = SHELLY_CHANGE_PROFILE;
    change.profile = profile;
    shelly_change_stage(&change);
    return true;
}

void shelly_change_cancel(void)
{
    pthread_mutex_lock(&s_lock);
    change_clear();
    pthread_mutex_unlock(&s_lock);
}

bool shelly_change_pending(shelly_change_t *out, uint32_t *secs_left)
{
    pthread_mutex_lock(&s_lock);
    bool live = change_live();
    if (live) {
        if (out != nullptr) {
            *out = s_change;
            // Only whether a password waits leaves this file, so no caller
            // holds a copy of it.
            if (out->wifi_password[0] != '\0') {
                memset(out->wifi_password, 0, sizeof(out->wifi_password));
                out->wifi_password[0] = '*';
            }
        }
        if (secs_left != nullptr) {
            int64_t left_us = (int64_t)SHELLY_CHANGE_CONFIRM_S * 1000000 - (esp_timer_get_time() - s_change_at_us);
            *secs_left = (uint32_t)((left_us + 999999) / 1000000);
        }
    }
    pthread_mutex_unlock(&s_lock);
    return live;
}

bool shelly_change_confirm(void)
{
    pthread_mutex_lock(&s_lock);
    bool ok = false;
    if (change_live()) {
        switch (s_change.kind) {
        case SHELLY_CHANGE_SETUP_CODE:
            ok = shelly_setup_code_save(&s_change.code);
            break;
        case SHELLY_CHANGE_SWITCH_MODE:
            ok = s_change.channel < RELAY_CHANNELS && save_switch_mode(s_change.channel, s_change.switch_mode);
            if (ok) {
                s_switch_mode[s_change.channel] = s_change.switch_mode;
#if RELAY_CHANNELS > 1
                ESP_LOGW(TAG, "Wall switch %u mode saved: %s", s_change.channel + 1,
                         ui_switch_mode_name(s_change.switch_mode));
#else
                ESP_LOGW(TAG, "Wall switch mode saved: %s", ui_switch_mode_name(s_change.switch_mode));
#endif
            }
            break;
        case SHELLY_CHANGE_CALIBRATION:
            ok = shelly_cal_save(&s_change.cal);
            break;
        case SHELLY_CHANGE_WIFI_PASSWORD:
            ok = shelly_wifi_password_save(s_change.wifi_password);
            break;
        case SHELLY_CHANGE_PROFILE: {
            load_profile();
            device_profile_t from = s_profile_saved;
            ok = save_profile(s_change.profile);
            if (ok) {
                s_profile_saved = s_change.profile;
                switch_mode_follow_profile(from, s_change.profile);
                ESP_LOGW(TAG, "Device type saved: %s, from the next normal boot",
                         device_profile_name(s_change.profile));
            }
            break;
        }
        case SHELLY_CHANGE_NONE:
        default:
            break;
        }
        if (ok) {
            change_clear();
            s_saved_count++;
        }
    }
    pthread_mutex_unlock(&s_lock);
    return ok;
}

uint32_t shelly_change_saved_count(void)
{
    pthread_mutex_lock(&s_lock);
    uint32_t n = s_saved_count;
    pthread_mutex_unlock(&s_lock);
    return n;
}
