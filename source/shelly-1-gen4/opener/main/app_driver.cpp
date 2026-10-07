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

#include <esp_log.h>
#include <esp_timer.h>

#include <esp_matter.h>
#include <app_priv.h>
#include <common_macros.h>

#include <relay.h>
#include <shelly_app.h>

using namespace chip::app::Clusters;
using namespace esp_matter;

static const char *TAG = "app_driver";

extern uint16_t relay_endpoint_id;

// Momentary pulse timer. On a trigger, arm a one-shot for OPENER_PULSE_MS,
// then turn the relay back off by clearing the OnOff attribute (not via
// relay_set, so the Matter-reported state returns to off too).
static esp_timer_handle_t s_pulse_timer = nullptr;

static void pulse_timer_cb(void *arg)
{
    esp_matter_attr_val_t val = esp_matter_bool(false);
    attribute::update(relay_endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &val);
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

esp_err_t app_driver_attribute_update(app_driver_handle_t driver_handle, uint16_t endpoint_id, uint32_t cluster_id,
                                      uint32_t attribute_id, esp_matter_attr_val_t *val)
{
    esp_err_t err = ESP_OK;
    if (endpoint_id == relay_endpoint_id) {
        if (cluster_id == OnOff::Id) {
            if (attribute_id == OnOff::Attributes::OnOff::Id) {
                // Momentary: never assert the trigger unless its release is
                // already guaranteed. Arm the auto-off first and energize only
                // if it armed, so a relay that cannot schedule its own release
                // never closes at all. If the relay then refuses to turn on
                // (thermal lockout), cancel the pending auto-off so it does not
                // fire a stray off later. Prevents the fail-open reported in #11.
                if (val->val.b) {
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

esp_err_t app_driver_relay_set_defaults(uint16_t endpoint_id)
{
    // Momentary: the trigger is never asserted at boot. A stored or
    // StartUpOnOff "on" would otherwise close the relay with no pulse
    // timer armed to release it. Force the relay and the attribute off.
    esp_err_t err = relay_set(false);
    shelly_onoff_set(endpoint_id, false);

    return err;
}
