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

// relay.cpp

#include "relay.h"

#include <esp_log.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <sdkconfig.h>

// SAFETY: This module is the single point of relay control. See relay.h
// for the full safety invariant and contributor convention. Briefly:
//   - relay_init() drives the GPIO LOW immediately after configuring it
//     as output, and before releasing the pad hold. Do not reorder.
//   - relay_set(true) is gated by every registered gate. Do not bypass.
//   - relay_set(false) and relay_trip() are always allowed. Do not gate them.

#ifndef CONFIG_SHELLY_RELAY_GPIO
#error "CONFIG_SHELLY_RELAY_GPIO must be declared in the variant's Kconfig.projbuild"
#endif

static const char *TAG = "relay";

// The relay is active high: HIGH = energized, LOW = de-energized.
#define RELAY_GPIO  ((gpio_num_t)CONFIG_SHELLY_RELAY_GPIO)
#define MAX_GATES   6

static SemaphoreHandle_t s_lock = nullptr;
static StaticSemaphore_t s_lock_buf;
static relay_gate_fn_t s_gates[MAX_GATES];
static int s_gate_count = 0;
static volatile bool s_level = false;
static bool s_ready = false;

esp_err_t relay_init(void)
{
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);

    gpio_config_t relay_cfg = {
        .pin_bit_mask = (1ULL << RELAY_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&relay_cfg);

    // SAFETY: Drive the relay LOW as the first action after output
    // configuration, then release any pad hold left by the previous
    // firmware (stock Shelly firmware holds the relay pad). The output
    // register already reads LOW when the hold releases, so the pin goes
    // straight to LOW. Drive LOW again once the pad follows the register.
    gpio_set_level(RELAY_GPIO, 0);
    gpio_hold_dis(RELAY_GPIO);
    gpio_set_level(RELAY_GPIO, 0);
    s_level = false;

    if (err != ESP_OK) {
        // The pin may not be an output. Refuse every turn-on rather than
        // trust a relay we could not configure.
        ESP_LOGE(TAG, "Failed to configure relay GPIO%d: %s", RELAY_GPIO, esp_err_to_name(err));
        return err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "Relay initialized on GPIO%d (de-energized)", RELAY_GPIO);
    return ESP_OK;
}

esp_err_t relay_register_gate(relay_gate_fn_t gate)
{
    if (s_gate_count >= MAX_GATES) {
        ESP_LOGE(TAG, "Relay gate table full");
        return ESP_ERR_NO_MEM;
    }
    s_gates[s_gate_count++] = gate;
    return ESP_OK;
}

static esp_err_t relay_write_locked(bool power)
{
    if (power) {
        if (!s_ready) {
            ESP_LOGW(TAG, "Relay not initialized, turn-on blocked");
            return ESP_ERR_INVALID_STATE;
        }
        // SAFETY: Refuse turn-on while any gate refuses. Turn-off is
        // always allowed. The safe direction is never blocked.
        for (int i = 0; i < s_gate_count; i++) {
            const char *reason = s_gates[i]();
            if (reason != nullptr) {
                ESP_LOGW(TAG, "Relay turn-on blocked: %s", reason);
                return ESP_ERR_INVALID_STATE;
            }
        }
    }

    ESP_LOGI(TAG, "Setting relay: %s", power ? "ON" : "OFF");
    esp_err_t err = gpio_set_level(RELAY_GPIO, power ? 1 : 0);
    if (err == ESP_OK) {
        s_level = power;
    }
    return err;
}

esp_err_t relay_set(bool power)
{
    if (s_lock == nullptr) {
        // Before relay_init(). Nothing may energize the relay yet.
        return power ? ESP_ERR_INVALID_STATE : ESP_OK;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = relay_write_locked(power);
    xSemaphoreGive(s_lock);
    return err;
}

void relay_trip(void)
{
    if (s_lock == nullptr) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    gpio_set_level(RELAY_GPIO, 0);
    s_level = false;
    xSemaphoreGive(s_lock);
    // Logged after the pin is low: a busy console must not delay the trip.
    ESP_LOGW(TAG, "Relay tripped");
}

bool relay_get(void)
{
    return s_level;
}
