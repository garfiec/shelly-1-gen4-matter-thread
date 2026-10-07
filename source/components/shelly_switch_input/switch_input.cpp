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

static const char *TAG = "switch_input";

// HARDWARE NOTE: GPIO_PULLUP_DISABLE and GPIO_PULLDOWN_DISABLE are
// required. The external circuit on the Shelly PCB defines the SW level.
// Enabling either internal pull will fight the external circuit and
// prevent the ISR from firing. Do not "fix" this by adding a pull.
#define SWITCH_INPUT_GPIO   ((gpio_num_t)CONFIG_SHELLY_SWITCH_GPIO)
#define DEBOUNCE_MS         50
#define QUEUE_DEPTH         10
// The change handler updates Matter attributes or sends bound commands,
// which run on this stack.
#define TASK_STACK_SIZE     4096
#define TASK_PRIORITY       10

// ISR-to-task communication queue. Written by the ISR (FromISR),
// read by the task. File-static so the ISR can reach it.
static QueueHandle_t switch_evt_queue = nullptr;
static switch_input_cb_t s_on_change = nullptr;
static void *s_on_change_arg = nullptr;
// The last settled level. Only the switch task writes it after init.
static bool s_stable_level = false;

static void IRAM_ATTR switch_input_isr(void *arg)
{
    uint32_t gpio_num = SWITCH_INPUT_GPIO;
    xQueueSendFromISR(switch_evt_queue, &gpio_num, nullptr);
}

bool switch_input_get_level(void)
{
    return gpio_get_level(SWITCH_INPUT_GPIO) != 0;
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
    while (true) {
        uint32_t io_num;
        if (xQueueReceive(switch_evt_queue, &io_num, portMAX_DELAY)) {
            // Debounce: wait, then drain any additional events that
            // queued during the debounce window. This collapses a noisy
            // mechanical edge into a single logical event.
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
            while (xQueueReceive(switch_evt_queue, &io_num, 0) == pdTRUE) {}

            // Act only on a real change of the settled level. A spike or
            // mains-coupled glitch that returns to the previous level fires
            // the ISR too, and must not toggle the relay.
            bool level = switch_input_get_level();
            if (level == s_stable_level) {
                ESP_LOGD(TAG, "Switch edge ignored, level unchanged");
                continue;
            }
            s_stable_level = level;
            ESP_LOGI(TAG, "Switch input changed: %s", level ? "HIGH" : "LOW");
            s_on_change(level, s_on_change_arg);
        }
    }
}

esp_err_t switch_input_init(switch_input_cb_t on_change, void *arg)
{
    s_on_change = on_change;
    s_on_change_arg = arg;

    // Configure the pin as an edge-interrupt input. See HARDWARE NOTE
    // above for why both internal pulls are disabled.
    gpio_config_t switch_cfg = {
        .pin_bit_mask = (1ULL << SWITCH_INPUT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    esp_err_t err = gpio_config(&switch_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure switch GPIO%d: %s", SWITCH_INPUT_GPIO, esp_err_to_name(err));
        return err;
    }
    s_stable_level = switch_input_get_level();

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

    esp_err_t handler_err = gpio_isr_handler_add(SWITCH_INPUT_GPIO,
                                                  switch_input_isr, nullptr);
    if (handler_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add switch ISR handler: %d", handler_err);
        return handler_err;
    }

    ESP_LOGI(TAG, "Switch input initialized on GPIO%d (level %s)", SWITCH_INPUT_GPIO,
             s_stable_level ? "HIGH" : "LOW");
    return ESP_OK;
}
