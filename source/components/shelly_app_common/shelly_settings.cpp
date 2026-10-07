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

#include <esp_log.h>
#include <esp_timer.h>
#include <nvs.h>
#include <sdkconfig.h>

#include "shelly_app.h"
#include "shelly_setup_code.h"

static const char *TAG = "settings";

static const char *NVS_NAMESPACE = "shelly_cfg";
static const char *NVS_KEY_SWITCH_MODE = "sw_mode";

// Guards the switch mode and the waiting change (the maintenance page and
// the button run on their own tasks). Taken before the setup code's own
// lock, never after it.
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

// Read by the switch task on every change: plain aligned words.
static volatile ui_switch_mode_t s_switch_mode = UI_SWITCH_MODE_TOGGLE;
static volatile bool s_loaded = false;

static shelly_change_t s_change;
static int64_t s_change_at_us = 0;

void shelly_settings_init(void)
{
    ui_switch_mode_t mode = UI_SWITCH_MODE_TOGGLE;
#if CONFIG_SHELLY_MAINTENANCE
    // Without Wi-Fi mode there is no page to change a saved mode back:
    // such a build always uses Toggle.
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, NVS_KEY_SWITCH_MODE, &v) == ESP_OK) {
            if (v < UI_SWITCH_MODE_COUNT) {
                mode = (ui_switch_mode_t)v;
            } else {
                ESP_LOGW(TAG, "Unknown saved switch mode %u: using toggle", v);
            }
        }
        nvs_close(h);
    }
#endif
    s_switch_mode = mode;
    s_loaded = true;
    ESP_LOGI(TAG, "Wall switch mode: %s", ui_switch_mode_name(mode));
}

ui_switch_mode_t shelly_switch_mode_get(void)
{
    // A switch change can arrive before shelly_ui_init(): load it then.
    if (!s_loaded) {
        shelly_settings_init();
    }
    return s_switch_mode;
}

static bool save_switch_mode(ui_switch_mode_t mode)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, NVS_KEY_SWITCH_MODE, (uint8_t)mode);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Saving the switch mode failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

// With s_lock held.
static bool change_live(void)
{
    if (s_change.kind == SHELLY_CHANGE_NONE) {
        return false;
    }
    if (esp_timer_get_time() - s_change_at_us >= (int64_t)SHELLY_CHANGE_CONFIRM_S * 1000000) {
        s_change.kind = SHELLY_CHANGE_NONE;
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

bool shelly_switch_mode_stage(ui_switch_mode_t mode)
{
    if (shelly_ui_switch_role() != UI_SWITCH_TOGGLE || (unsigned)mode >= UI_SWITCH_MODE_COUNT) {
        return false;
    }
    shelly_change_t change = {};
    change.kind = SHELLY_CHANGE_SWITCH_MODE;
    change.switch_mode = mode;
    shelly_change_stage(&change);
    return true;
}

void shelly_change_cancel(void)
{
    pthread_mutex_lock(&s_lock);
    s_change.kind = SHELLY_CHANGE_NONE;
    pthread_mutex_unlock(&s_lock);
}

bool shelly_change_pending(shelly_change_t *out, uint32_t *secs_left)
{
    pthread_mutex_lock(&s_lock);
    bool live = change_live();
    if (live) {
        if (out != nullptr) {
            *out = s_change;
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
            ok = save_switch_mode(s_change.switch_mode);
            if (ok) {
                s_switch_mode = s_change.switch_mode;
                ESP_LOGW(TAG, "Wall switch mode saved: %s", ui_switch_mode_name(s_change.switch_mode));
            }
            break;
        case SHELLY_CHANGE_NONE:
        default:
            break;
        }
        if (ok) {
            s_change.kind = SHELLY_CHANGE_NONE;
        }
    }
    pthread_mutex_unlock(&s_lock);
    return ok;
}
