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

// switch_input.cpp

#include "switch_input.h"

#include <esp_log.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <sdkconfig.h>

#ifndef CONFIG_SHELLY_SWITCH_GPIO
#error "CONFIG_SHELLY_SWITCH_GPIO must be declared in the variant's Kconfig.projbuild"
#endif
#if SWITCH_INPUT_CHANNELS > 1 && !defined(CONFIG_SHELLY_SWITCH2_GPIO)
#error "CONFIG_SHELLY_SWITCH2_GPIO must be declared for a 2-channel board"
#endif

static const char *TAG = "switch_input";

// HARDWARE NOTE: GPIO_PULLUP_DISABLE and GPIO_PULLDOWN_DISABLE are
// required. The external circuit on the Shelly PCB defines the SW level.
// Enabling either internal pull will fight the external circuit and
// prevent the ISR from firing. Do not "fix" this by adding a pull.
#define DEBOUNCE_MS         50
#define QUEUE_DEPTH         10
// The change handler updates Matter attributes or sends bound commands,
// which run on this stack.
#define TASK_STACK_SIZE     4096
#define TASK_PRIORITY       10

static const gpio_num_t s_pins[SWITCH_INPUT_CHANNELS] = {
    (gpio_num_t)CONFIG_SHELLY_SWITCH_GPIO,
#if SWITCH_INPUT_CHANNELS > 1
    (gpio_num_t)CONFIG_SHELLY_SWITCH2_GPIO,
#endif
};

// ISR-to-task communication queue. Written by the ISR (FromISR),
// read by the task. File-static so the ISR can reach it.
static QueueHandle_t switch_evt_queue = nullptr;
static switch_input_cb_t s_on_change = nullptr;
static switch_input_ch_cb_t s_on_change_ch = nullptr;
static void *s_on_change_arg = nullptr;
// The last settled levels. Only the switch task writes them after init.
static bool s_stable_level[SWITCH_INPUT_CHANNELS];
// Raw edges seen by the ISR, for diagnostics.
static volatile uint32_t s_edges[SWITCH_INPUT_CHANNELS];
// Per input: an edge waits to be settled, and the tick of the first edge
// of that wait. Each input is debounced from its own first edge.
static portMUX_TYPE s_pending_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_pending[SWITCH_INPUT_CHANNELS];
static TickType_t s_pending_since[SWITCH_INPUT_CHANNELS];

static void IRAM_ATTR switch_input_isr(void *arg)
{
    uint32_t ch = (uint32_t)(uintptr_t)arg;
    s_edges[ch]++;
    portENTER_CRITICAL_ISR(&s_pending_mux);
    if (!s_pending[ch]) {
        s_pending[ch] = true;
        s_pending_since[ch] = xTaskGetTickCountFromISR();
    }
    portEXIT_CRITICAL_ISR(&s_pending_mux);
    xQueueSendFromISR(switch_evt_queue, &ch, nullptr);
}

bool switch_input_get_level_ch(int ch)
{
    return ch >= 0 && ch < SWITCH_INPUT_CHANNELS && gpio_get_level(s_pins[ch]) != 0;
}

bool switch_input_get_level(void)
{
    return switch_input_get_level_ch(0);
}

bool switch_input_stable_level_ch(int ch)
{
    return ch >= 0 && ch < SWITCH_INPUT_CHANNELS && s_stable_level[ch];
}

uint32_t switch_input_edge_count(int ch)
{
    return ch >= 0 && ch < SWITCH_INPUT_CHANNELS ? s_edges[ch] : 0;
}

bool switch_input_active(bool level)
{
#if CONFIG_SHELLY_SWITCH_ACTIVE_HIGH
    return level;
#else
    return !level;
#endif
}

static void switch_input_task(void *arg)
{
    const TickType_t debounce = pdMS_TO_TICKS(DEBOUNCE_MS);
    while (true) {
        // Sleep until an edge, or until the oldest waiting input has been
        // waiting DEBOUNCE_MS since its own first edge. The queue only wakes
        // the task: the pending flags say which inputs to read.
        TickType_t wait = portMAX_DELAY;
        TickType_t now = xTaskGetTickCount();
        taskENTER_CRITICAL(&s_pending_mux);
        for (int i = 0; i < SWITCH_INPUT_CHANNELS; i++) {
            if (s_pending[i]) {
                TickType_t age = now - s_pending_since[i];
                TickType_t left = age >= debounce ? 0 : debounce - age;
                if (left < wait) {
                    wait = left;
                }
            }
        }
        taskEXIT_CRITICAL(&s_pending_mux);
        uint32_t ch;
        if (wait > 0) {
            xQueueReceive(switch_evt_queue, &ch, wait);
        }

        // Act only on a real change of the settled level. A spike or
        // mains-coupled glitch that returns to the previous level fires
        // the ISR too, and must not toggle the relay. Each input is read
        // only once its own edge is DEBOUNCE_MS old: a noisy mechanical
        // edge collapses into one event, and another input's edge never
        // shortens it.
        now = xTaskGetTickCount();
        for (int i = 0; i < SWITCH_INPUT_CHANNELS; i++) {
            taskENTER_CRITICAL(&s_pending_mux);
            bool due = s_pending[i] && now - s_pending_since[i] >= debounce;
            if (due) {
                s_pending[i] = false;
            }
            taskEXIT_CRITICAL(&s_pending_mux);
            if (!due) {
                continue;
            }
            bool level = switch_input_get_level_ch(i);
            if (level == s_stable_level[i]) {
                ESP_LOGD(TAG, "Switch edge ignored, level unchanged");
                continue;
            }
            s_stable_level[i] = level;
#if SWITCH_INPUT_CHANNELS > 1
            ESP_LOGI(TAG, "Switch input %d changed: %s", i + 1, level ? "HIGH" : "LOW");
#else
            ESP_LOGI(TAG, "Switch input changed: %s", level ? "HIGH" : "LOW");
#endif
            if (s_on_change_ch != nullptr) {
                s_on_change_ch(i, level, s_on_change_arg);
            } else if (i == 0) {
                s_on_change(level, s_on_change_arg);
            }
        }
    }
}

static esp_err_t start(void)
{
    // Configure the pins as edge-interrupt inputs. See HARDWARE NOTE
    // above for why both internal pulls are disabled.
    uint64_t mask = 0;
    for (int i = 0; i < SWITCH_INPUT_CHANNELS; i++) {
        mask |= 1ULL << s_pins[i];
    }
    gpio_config_t switch_cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    esp_err_t err = gpio_config(&switch_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure switch GPIOs: %s", esp_err_to_name(err));
        return err;
    }
    for (int i = 0; i < SWITCH_INPUT_CHANNELS; i++) {
        s_stable_level[i] = switch_input_get_level_ch(i);
    }

    // Create the ISR-to-task communication queue and the task that
    // services it. Both must exist before the ISR is attached.
    switch_evt_queue = xQueueCreate(QUEUE_DEPTH, sizeof(uint32_t));
    if (switch_evt_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create switch queue");
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(switch_input_task, "switch_input", TASK_STACK_SIZE,
                    nullptr, TASK_PRIORITY, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create switch task");
        return ESP_ERR_NO_MEM;
    }

    // Install the GPIO ISR service. ESP_ERR_INVALID_STATE means it was
    // already installed by another module (the Matter stack or
    // iot_button), which is fine — we share a single ISR service across
    // the whole app.
    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to install ISR service: %d", isr_err);
        return isr_err;
    }

    for (int i = 0; i < SWITCH_INPUT_CHANNELS; i++) {
        esp_err_t handler_err = gpio_isr_handler_add(s_pins[i], switch_input_isr, (void *)(uintptr_t)i);
        if (handler_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to add switch ISR handler: %d", handler_err);
            return handler_err;
        }
#if SWITCH_INPUT_CHANNELS > 1
        ESP_LOGI(TAG, "Switch input %d initialized on GPIO%d (level %s)", i + 1, s_pins[i],
                 s_stable_level[i] ? "HIGH" : "LOW");
#else
        ESP_LOGI(TAG, "Switch input initialized on GPIO%d (level %s)", s_pins[i],
                 s_stable_level[i] ? "HIGH" : "LOW");
#endif
    }
    return ESP_OK;
}

esp_err_t switch_input_init(switch_input_cb_t on_change, void *arg)
{
    s_on_change = on_change;
    s_on_change_arg = arg;
    return start();
}

esp_err_t switch_input_init_ch(switch_input_ch_cb_t on_change, void *arg)
{
    s_on_change_ch = on_change;
    s_on_change_arg = arg;
    return start();
}
