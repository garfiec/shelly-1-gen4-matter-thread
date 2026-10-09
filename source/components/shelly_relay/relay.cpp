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
//   - relay_set(false), relay_trip() and relay_trip_ch() are always allowed.
//     Do not gate them.

#ifndef CONFIG_SHELLY_RELAY_GPIO
#error "CONFIG_SHELLY_RELAY_GPIO must be declared in the variant's Kconfig.projbuild"
#endif
#if CONFIG_SHELLY_CHANNELS > 1 && !defined(CONFIG_SHELLY_RELAY2_GPIO)
#error "CONFIG_SHELLY_RELAY2_GPIO must be declared for a 2-channel board"
#endif

static const char *TAG = "relay";

// The relays are active high: HIGH = energized, LOW = de-energized.
#define MAX_GATES   6

static const gpio_num_t s_pins[RELAY_CHANNELS] = {
    (gpio_num_t)CONFIG_SHELLY_RELAY_GPIO,
#if CONFIG_SHELLY_CHANNELS > 1
    (gpio_num_t)CONFIG_SHELLY_RELAY2_GPIO,
#endif
};

static SemaphoreHandle_t s_lock = nullptr;
static StaticSemaphore_t s_lock_buf;
static relay_gate_fn_t s_gates[MAX_GATES];
static int s_gate_count = 0;
static volatile bool s_level[RELAY_CHANNELS];
static bool s_ready = false;

esp_err_t relay_init(void)
{
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);

    uint64_t mask = 0;
    for (int ch = 0; ch < RELAY_CHANNELS; ch++) {
        mask |= 1ULL << s_pins[ch];
    }
    gpio_config_t relay_cfg = {
        .pin_bit_mask = mask,
#if CONFIG_SHELLY_RELAY_READBACK
        .mode = GPIO_MODE_INPUT_OUTPUT,
#else
        .mode = GPIO_MODE_OUTPUT,
#endif
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&relay_cfg);

    // SAFETY: Drive each relay LOW as the first action after output
    // configuration, then release any pad hold left by the previous
    // firmware (stock Shelly firmware holds the relay pad). The output
    // register already reads LOW when the hold releases, so the pin goes
    // straight to LOW. Drive LOW again once the pad follows the register.
    for (int ch = 0; ch < RELAY_CHANNELS; ch++) {
        gpio_set_level(s_pins[ch], 0);
        gpio_hold_dis(s_pins[ch]);
        gpio_set_level(s_pins[ch], 0);
        s_level[ch] = false;
    }

    if (err != ESP_OK) {
        // A pin may not be an output. Refuse every turn-on rather than
        // trust a relay we could not configure.
        ESP_LOGE(TAG, "Failed to configure relay GPIOs: %s", esp_err_to_name(err));
        return err;
    }

    s_ready = true;
#if CONFIG_SHELLY_CHANNELS > 1
    for (int ch = 0; ch < RELAY_CHANNELS; ch++) {
        ESP_LOGI(TAG, "Relay %d initialized on GPIO%d (de-energized)", ch + 1, s_pins[ch]);
    }
#else
    ESP_LOGI(TAG, "Relay initialized on GPIO%d (de-energized)", s_pins[0]);
#endif
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

static esp_err_t relay_write_locked(int ch, bool power)
{
    if (power) {
        if (!s_ready) {
            ESP_LOGW(TAG, "Relay not initialized, turn-on blocked");
            return ESP_ERR_INVALID_STATE;
        }
        // SAFETY: Refuse turn-on while any gate refuses. Turn-off is
        // always allowed. The safe direction is never blocked.
        for (int i = 0; i < s_gate_count; i++) {
            const char *reason = s_gates[i](ch);
            if (reason != nullptr) {
                ESP_LOGW(TAG, "Relay turn-on blocked: %s", reason);
                return ESP_ERR_INVALID_STATE;
            }
        }
    }

#if CONFIG_SHELLY_CHANNELS > 1
    ESP_LOGI(TAG, "Setting relay %d: %s", ch + 1, power ? "ON" : "OFF");
#else
    ESP_LOGI(TAG, "Setting relay: %s", power ? "ON" : "OFF");
#endif
    esp_err_t err = gpio_set_level(s_pins[ch], power ? 1 : 0);
    if (err == ESP_OK) {
        s_level[ch] = power;
    }
    return err;
}

esp_err_t relay_set_ch(int ch, bool power)
{
    if (ch < 0 || ch >= RELAY_CHANNELS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == nullptr) {
        // Before relay_init(). Nothing may energize the relay yet.
        return power ? ESP_ERR_INVALID_STATE : ESP_OK;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = relay_write_locked(ch, power);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t relay_set(bool power)
{
    return relay_set_ch(0, power);
}

void relay_trip(void)
{
    if (s_lock == nullptr) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int ch = 0; ch < RELAY_CHANNELS; ch++) {
        gpio_set_level(s_pins[ch], 0);
        s_level[ch] = false;
    }
    xSemaphoreGive(s_lock);
    // Logged after the pins are low: a busy console must not delay the trip.
    ESP_LOGW(TAG, "Relay tripped");
}

void relay_trip_ch(int ch)
{
    if (s_lock == nullptr || ch < 0 || ch >= RELAY_CHANNELS) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    gpio_set_level(s_pins[ch], 0);
    s_level[ch] = false;
    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "Relay %d tripped", ch + 1);
}

bool relay_get_ch(int ch)
{
    return ch >= 0 && ch < RELAY_CHANNELS && s_level[ch];
}

bool relay_get(void)
{
    return relay_get_ch(0);
}

uint32_t relay_get_mask(void)
{
    uint32_t mask = 0;
    for (int ch = 0; ch < RELAY_CHANNELS; ch++) {
        if (s_level[ch]) {
            mask |= 1u << ch;
        }
    }
    return mask;
}

uint32_t relay_pin_mask(void)
{
#if CONFIG_SHELLY_RELAY_READBACK
    uint32_t mask = 0;
    for (int ch = 0; ch < RELAY_CHANNELS; ch++) {
        if (gpio_get_level(s_pins[ch])) {
            mask |= 1u << ch;
        }
    }
    return mask;
#else
    return relay_get_mask();
#endif
}
