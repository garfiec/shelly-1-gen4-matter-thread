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



// shelly_pulse.cpp

#include "shelly_pulse.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <sdkconfig.h>

#include <relay.h>

#include "shelly_app.h"

static const char *TAG = "pulse";

#ifndef CONFIG_SHELLY_PULSE_MS
#define CONFIG_SHELLY_PULSE_MS 500
#endif

// ---- Through Matter ----

static esp_timer_handle_t s_pulse_timer = nullptr;
static volatile uint16_t s_pulse_endpoint = 0;

static void pulse_timer_cb(void *arg)
{
    // Release the trigger here, on time: the attribute update waits for the
    // CHIP stack lock, which the Matter thread can hold for seconds, and it
    // runs on the Matter thread instead of blocking the esp_timer task.
    relay_set(false);
    shelly_onoff_set_async(s_pulse_endpoint, false);
}

static esp_err_t pulse_timer_arm(void)
{
    if (s_pulse_timer == nullptr) {
        const esp_timer_create_args_t args = {
            .callback = pulse_timer_cb,
            .name = "pulse",
        };
        esp_err_t err = esp_timer_create(&args, &s_pulse_timer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create the pulse timer");
            return err;
        }
    }
    // Restart cleanly if a pulse is still running. Stopping a timer that
    // isn't running fails as expected: only the start decides.
    esp_timer_stop(s_pulse_timer);
    return esp_timer_start_once(s_pulse_timer, (uint64_t)CONFIG_SHELLY_PULSE_MS * 1000);
}

esp_err_t shelly_pulse_write(uint16_t endpoint_id, bool on)
{
    if (!on) {
        if (s_pulse_timer != nullptr) {
            esp_timer_stop(s_pulse_timer);
        }
        return relay_set(false);
    }
    // A StartUpOnOff of On or Toggle is applied inside esp_matter::start()
    // through this same write, and would move the door at every boot.
    if (!shelly_app_is_started()) {
        ESP_LOGW(TAG, "Trigger refused: Matter is not up yet");
        return ESP_ERR_INVALID_STATE;
    }
    s_pulse_endpoint = endpoint_id;
    esp_err_t err = pulse_timer_arm();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Pulse release unavailable, relay not closed");
        return err;
    }
    err = relay_set(true);
    if (err != ESP_OK) {
        // Refused (over-temperature): no stray release later.
        esp_timer_stop(s_pulse_timer);
    }
    return err;
}

esp_err_t shelly_pulse_set_defaults(uint16_t endpoint_id)
{
    esp_err_t err = relay_set(false);
    shelly_onoff_set(endpoint_id, false);
    return err;
}

// ---- Without Matter (Wi-Fi maintenance mode) ----

// The button task and the page's Toggle (the web server task) both call
// these, so they run under a lock.
static esp_timer_handle_t s_local_timer = nullptr;
static SemaphoreHandle_t s_local_lock = nullptr;
static StaticSemaphore_t s_local_lock_buf;

static void local_pulse_end(void *arg)
{
    relay_set(false);
}

esp_err_t shelly_pulse_local_init(void)
{
    s_local_lock = xSemaphoreCreateMutexStatic(&s_local_lock_buf);
    const esp_timer_create_args_t args = {
        .callback = local_pulse_end,
        .name = "pulse_local",
    };
    esp_err_t err = esp_timer_create(&args, &s_local_timer);
    if (err != ESP_OK) {
        s_local_timer = nullptr;
        ESP_LOGE(TAG, "Failed to create the local pulse timer: the trigger is refused");
    }
    return err;
}

// With s_local_lock held: start a pulse, arming the release first.
static bool local_start(void)
{
    if (esp_timer_start_once(s_local_timer, (uint64_t)CONFIG_SHELLY_PULSE_MS * 1000) != ESP_OK) {
        return false;
    }
    if (relay_set(true) != ESP_OK) {
        esp_timer_stop(s_local_timer);
        return false;
    }
    return true;
}

bool shelly_pulse_local(void)
{
    if (s_local_timer == nullptr) {
        return false;
    }
    xSemaphoreTake(s_local_lock, portMAX_DELAY);
    esp_timer_stop(s_local_timer);
    bool ok = relay_get() ? relay_set(false) == ESP_OK : local_start();
    xSemaphoreGive(s_local_lock);
    return ok;
}

bool shelly_pulse_local_to(bool on)
{
    if (s_local_timer == nullptr) {
        return false;
    }
    xSemaphoreTake(s_local_lock, portMAX_DELAY);
    bool ok;
    if (relay_get() == on) {
        ok = true;
    } else if (on) {
        esp_timer_stop(s_local_timer);
        ok = local_start();
    } else {
        esp_timer_stop(s_local_timer);
        ok = relay_set(false) == ESP_OK;
    }
    xSemaphoreGive(s_local_lock);
    return ok;
}
