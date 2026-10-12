//
// Copyright 2026 AUTOMATOUS.IO
// Portions derived from Espressif esp-matter examples,
// originally released into the public domain / CC0.
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

// app_driver.cpp
//
// The relay driver for every device type: latching for Light, Outlet and
// Light Switch, a momentary pulse for the Opener.

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <esp_matter.h>
#include <app_priv.h>
#include <common_macros.h>

#include <relay.h>
#include <shelly_app.h>
#include <shelly_settings.h>

using namespace chip::app::Clusters;
using namespace esp_matter;

static const char *TAG = "app_driver";

extern uint16_t relay_endpoint_id;

// Momentary pulse timer. On a trigger, arm a one-shot for OPENER_PULSE_MS,
// then release the relay and clear the OnOff attribute, so the
// Matter-reported state returns to off too.
static esp_timer_handle_t s_pulse_timer = nullptr;

static void pulse_timer_cb(void *arg)
{
    // Release the trigger here, on time: the attribute update waits for the
    // CHIP stack lock, which the Matter thread can hold for seconds, and it
    // runs on the Matter thread instead of blocking the esp_timer task.
    relay_set(false);
    shelly_onoff_set_async(relay_endpoint_id, false);
}

static esp_err_t pulse_timer_arm(void)
{
    if (s_pulse_timer == nullptr) {
        const esp_timer_create_args_t args = {
            .callback = pulse_timer_cb,
            .name = "opener_pulse",
        };
        esp_err_t err = esp_timer_create(&args, &s_pulse_timer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create pulse timer");
            return err;
        }
    }

    // Restart cleanly if a previous pulse is still pending. A stop on a timer
    // that is not running returns an error that is expected here, so only the
    // start result determines whether the auto-off is actually armed.
    esp_timer_stop(s_pulse_timer);
    return esp_timer_start_once(s_pulse_timer, (uint64_t)OPENER_PULSE_MS * 1000);
}

// The type is fixed for the boot: read it once, not on every OnOff write.
static bool is_opener(void)
{
    static const bool opener = shelly_profile_get() == DEVICE_PROFILE_OPENER;
    return opener;
}

esp_err_t app_driver_attribute_update(app_driver_handle_t driver_handle, uint16_t endpoint_id, uint32_t cluster_id,
                                      uint32_t attribute_id, esp_matter_attr_val_t *val)
{
    esp_err_t err = ESP_OK;
    if (endpoint_id == relay_endpoint_id) {
        if (cluster_id == OnOff::Id) {
            if (attribute_id == OnOff::Attributes::OnOff::Id) {
                if (!is_opener()) {
                    // Light, Outlet, Light Switch: latching.
                    err = relay_set(val->val.b);
                } else if (val->val.b) {
                    // Opener. Momentary: never assert the trigger unless its
                    // release is already guaranteed. Arm the auto-off first
                    // and energize only if it armed, so a relay that cannot
                    // schedule its own release never closes at all. If the
                    // relay then refuses to turn on (thermal lockout), cancel
                    // the pending auto-off so it does not fire a stray off
                    // later. Prevents the fail-open reported in #11.
                    //
                    // No trigger before Matter is up. A StartUpOnOff of On or
                    // Toggle is applied inside esp_matter::start() through
                    // this same write, and would move the door at every boot.
                    if (!shelly_app_is_started()) {
                        ESP_LOGW(TAG, "Trigger refused: Matter is not up yet");
                        return ESP_ERR_INVALID_STATE;
                    }
                    err = pulse_timer_arm();
                    if (err == ESP_OK) {
                        err = relay_set(true);
                        if (err != ESP_OK) {
                            esp_timer_stop(s_pulse_timer);
                        }
                    } else {
                        ESP_LOGE(TAG, "Pulse auto-off unavailable, relay not energized");
                    }
                } else {
                    err = relay_set(false);
                }
            }
        }
    }
    return err;
}

// The same pulse without Matter, for Wi-Fi maintenance mode: its own timer
// turns the relay off directly. A tap during a pulse ends it. The button
// task and the page's Toggle (the web server task) both call it, so it runs
// under a lock.
static esp_timer_handle_t s_local_pulse_timer = nullptr;
static SemaphoreHandle_t s_local_pulse_lock = nullptr;
static StaticSemaphore_t s_local_pulse_lock_buf;

static void local_pulse_end(void *arg)
{
    relay_set(false);
}

esp_err_t app_driver_local_pulse_init(void)
{
    s_local_pulse_lock = xSemaphoreCreateMutexStatic(&s_local_pulse_lock_buf);
    const esp_timer_create_args_t args = {
        .callback = local_pulse_end,
        .name = "opener_local_pulse",
    };
    esp_err_t err = esp_timer_create(&args, &s_local_pulse_timer);
    if (err != ESP_OK) {
        s_local_pulse_timer = nullptr;
        ESP_LOGE(TAG, "Failed to create the local pulse timer: the trigger is refused");
    }
    return err;
}

bool app_driver_local_pulse(void)
{
    if (s_local_pulse_timer == nullptr) {
        return false;
    }
    xSemaphoreTake(s_local_pulse_lock, portMAX_DELAY);
    bool ok;
    esp_timer_stop(s_local_pulse_timer);
    if (relay_get()) {
        ok = relay_set(false) == ESP_OK;
    } else if (esp_timer_start_once(s_local_pulse_timer, (uint64_t)OPENER_PULSE_MS * 1000) != ESP_OK) {
        // Arm the release first, as for a Matter trigger.
        ok = false;
    } else if (relay_set(true) != ESP_OK) {
        esp_timer_stop(s_local_pulse_timer);
        ok = false;
    } else {
        ok = true;
    }
    xSemaphoreGive(s_local_pulse_lock);
    return ok;
}

esp_err_t app_driver_relay_set_defaults(uint16_t endpoint_id)
{
    if (is_opener()) {
        // Momentary: the trigger is never asserted at boot. A StartUpOnOff of
        // On or Toggle is refused by app_driver_attribute_update() while Matter
        // starts; a stored "on" (a restart mid-pulse) is cleared here. Force the
        // relay and the attribute off.
        esp_err_t err = relay_set(false);
        shelly_onoff_set(endpoint_id, false);
        return err;
    }

    esp_err_t err = ESP_OK;
    esp_matter_attr_val_t val = esp_matter_invalid(NULL);

    attribute_t *attribute = attribute::get(endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id);
    attribute::get_val(attribute, &val);
    err = relay_set(val.val.b);
    if (err != ESP_OK && val.val.b) {
        // Turn-on refused (thermal lockout): report the relay as off.
        shelly_onoff_set(endpoint_id, false);
    }

    return err;
}
