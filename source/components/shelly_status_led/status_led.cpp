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

// status_led.cpp

#include "status_led.h"
#include "led_patterns.h"

#include <esp_log.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>

#ifndef CONFIG_SHELLY_LED_GPIO
#error "CONFIG_SHELLY_LED_GPIO must be declared in the variant's Kconfig.projbuild"
#endif

static const char *TAG = "status_led";

#define STATUS_LED_GPIO     ((gpio_num_t)CONFIG_SHELLY_LED_GPIO)
#ifdef CONFIG_SHELLY_LED_ACTIVE_LOW
#define LED_ON_LEVEL        0   // Active-low: pin LOW = LED ON
#define LED_OFF_LEVEL       1   // Active-low: pin HIGH = LED OFF
#else
#define LED_ON_LEVEL        1
#define LED_OFF_LEVEL       0
#endif

#define LED_TICK_MS         50  // Pattern resolution

// Layers, highest priority first: hold hint > one-shot > identify > fault >
// Matter update > base, except that the Wi-Fi maintenance states outrank a
// fault. A fault wins over the update pattern: a trip must stay visible. The
// task picks the top active layer each tick and restarts the pattern's
// timing whenever the shown pattern changes, so a hold hint starts on time
// rather than part-way through a blink.
static volatile led_state_t base_state = LED_STATE_OFF;
static volatile bool identify_active = false;
static volatile bool fault_active = false;
static volatile bool updating_active = false;
static volatile led_hint_t hint = LED_HINT_NONE;

// One-shot flash: set by any task, cleared by the LED task when it ends.
// The sequence number restarts a flash that replaces one still playing.
static portMUX_TYPE s_oneshot_mux = portMUX_INITIALIZER_UNLOCKED;
static led_pattern_id_t s_oneshot = LED_PAT_COUNT;  // LED_PAT_COUNT = none
static uint32_t s_oneshot_seq = 0;

static led_pattern_id_t base_pattern(led_state_t state)
{
    switch (state) {
    case LED_STATE_BLE_ADVERTISING:
        return LED_PAT_PAIRING;
    case LED_STATE_UNPAIRED_IDLE:
        return LED_PAT_UNPAIRED_IDLE;
    case LED_STATE_THREAD_CONNECTING:
        return LED_PAT_CONNECTING;
    case LED_STATE_THREAD_CONNECTED:
        return LED_PAT_CONNECTED;
    case LED_STATE_MAINTENANCE:
        return LED_PAT_MAINTENANCE;
    case LED_STATE_UPLOADING:
        return LED_PAT_UPLOADING;
    case LED_STATE_OFF:
    default:
        return LED_PAT_OFF;
    }
}

static led_pattern_id_t hint_pattern(led_hint_t h)
{
    switch (h) {
    case LED_HINT_HOLD_WAIT:
        return LED_PAT_HOLD_WAIT;
    case LED_HINT_HOLD_WIFI:
        return LED_PAT_HOLD_WIFI;
    case LED_HINT_HOLD_RESET:
        return LED_PAT_HOLD_RESET;
    case LED_HINT_NONE:
    default:
        return LED_PAT_COUNT;
    }
}

static void status_led_task(void *arg)
{
    led_pattern_id_t shown = LED_PAT_COUNT;
    uint32_t shown_seq = 0;
    uint32_t started = 0;

    while (true) {
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

        led_pattern_id_t oneshot;
        uint32_t seq;
        portENTER_CRITICAL(&s_oneshot_mux);
        oneshot = s_oneshot;
        seq = s_oneshot_seq;
        portEXIT_CRITICAL(&s_oneshot_mux);

        led_pattern_id_t want = hint_pattern(hint);
        if (want == LED_PAT_COUNT) {
            want = oneshot;
        }
        if (want == LED_PAT_COUNT) {
            // Wi-Fi maintenance mode shows over a fault: a faulted device (a
            // 1PM on bench power, a latched trip) is the one most likely to
            // need it, and the user must see that the mode is running.
            led_state_t state = base_state;
            bool wifi_mode = state == LED_STATE_MAINTENANCE || state == LED_STATE_UPLOADING;
            if (identify_active) {
                want = LED_PAT_IDENTIFY;
            } else if (fault_active && !wifi_mode) {
                want = LED_PAT_FAULT;
            } else if (updating_active) {
                want = LED_PAT_UPDATING;
            } else {
                want = base_pattern(state);
            }
        }

        if (want != shown || (want == oneshot && seq != shown_seq)) {
            shown = want;
            shown_seq = seq;
            started = now;
        }

        bool done = false;
        bool led_on = led_pattern_level(&LED_PATTERNS[shown], now - started, &done);
        if (done && shown == oneshot) {
            // Clear it only if no newer one-shot arrived meanwhile.
            portENTER_CRITICAL(&s_oneshot_mux);
            if (s_oneshot_seq == seq) {
                s_oneshot = LED_PAT_COUNT;
            }
            portEXIT_CRITICAL(&s_oneshot_mux);
            continue;  // Pick the next layer now, without a tick of stale level
        }

        gpio_set_level(STATUS_LED_GPIO, led_on ? LED_ON_LEVEL : LED_OFF_LEVEL);
        vTaskDelay(pdMS_TO_TICKS(LED_TICK_MS));
    }
}

static void oneshot_start(led_pattern_id_t id)
{
    portENTER_CRITICAL(&s_oneshot_mux);
    s_oneshot = id;
    s_oneshot_seq++;
    portEXIT_CRITICAL(&s_oneshot_mux);
}

esp_err_t status_led_init(void)
{
    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << STATUS_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&led_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure LED GPIO: %d", err);
        return err;
    }

    // Start with LED off, then release any pad hold left by the previous
    // firmware (stock Shelly firmware holds the LED pad). A held pad
    // ignores writes until released.
    gpio_set_level(STATUS_LED_GPIO, LED_OFF_LEVEL);
    gpio_hold_dis(STATUS_LED_GPIO);
    gpio_set_level(STATUS_LED_GPIO, LED_OFF_LEVEL);

    // Spawn LED task
    BaseType_t result = xTaskCreate(status_led_task, "status_led", 2048, NULL, 5, NULL);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create LED task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Status LED initialized on GPIO%d", STATUS_LED_GPIO);
    return ESP_OK;
}

void status_led_set_state(led_state_t state)
{
    if (state != base_state) {
        ESP_LOGI(TAG, "LED state change: %d -> %d", base_state, state);
        base_state = state;
    }
}

void status_led_start_identify(void)
{
    if (identify_active) {
        ESP_LOGI(TAG, "Identify already active - extending");
    } else {
        ESP_LOGI(TAG, "Identify start");
    }
    identify_active = true;
}

void status_led_stop_identify(void)
{
    if (identify_active) {
        ESP_LOGI(TAG, "Identify stop - restoring %s", fault_active ? "fault" : "state");
    } else {
        ESP_LOGI(TAG, "Identify stop called but not identifying");
    }
    identify_active = false;
}

void status_led_set_hint(led_hint_t h)
{
    hint = h;
}

void status_led_flash_press(void)
{
    oneshot_start(LED_PAT_PRESS);
}

void status_led_flash_refused(void)
{
    ESP_LOGI(TAG, "Refused");
    oneshot_start(LED_PAT_REFUSED);
}

void status_led_flash_accepted(void)
{
    ESP_LOGI(TAG, "Accepted");
    oneshot_start(LED_PAT_ACCEPTED);
}

void status_led_set_updating(bool active)
{
    if (active != updating_active) {
        ESP_LOGI(TAG, "Update pattern %s", active ? "on" : "off");
        updating_active = active;
    }
}

void status_led_set_fault(bool active)
{
    if (active != fault_active) {
        ESP_LOGI(TAG, "Fault pattern %s", active ? "on" : "off");
        fault_active = active;
    }
}
