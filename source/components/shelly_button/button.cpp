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

// button.cpp

#include "button.h"

#include <esp_log.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <button_gpio.h>
#include <iot_button.h>
#include <sdkconfig.h>

#ifndef CONFIG_SHELLY_BUTTON_GPIO
#error "CONFIG_SHELLY_BUTTON_GPIO must be declared in the variant's Kconfig.projbuild"
#endif

static const char *TAG = "button";

#define BUTTON_GPIO         CONFIG_SHELLY_BUTTON_GPIO
#ifdef CONFIG_SHELLY_BUTTON_ACTIVE_LOW
#define BUTTON_ACTIVE_LEVEL 0
#else
#define BUTTON_ACTIVE_LEVEL 1
#endif
#define QUEUE_DEPTH         4
// The press handler updates Matter attributes, which run the data model
// and reporting on this stack.
#define TASK_STACK_SIZE     4096
#define TASK_PRIORITY       10

static QueueHandle_t s_press_queue = nullptr;
static shelly_button_cb_t s_on_press = nullptr;
static void *s_on_press_arg = nullptr;

// Runs in the esp_timer task. Hand the press to the button task so the
// handler never blocks the timer task on the CHIP stack lock.
static void button_press_cb(void *arg, void *data)
{
    uint8_t press = 1;
    if (xQueueSend(s_press_queue, &press, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Button press dropped, queue full");
    }
}

static void button_task(void *arg)
{
    uint8_t press;
    while (true) {
        if (xQueueReceive(s_press_queue, &press, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "Button pressed");
            s_on_press(s_on_press_arg);
        }
    }
}

shelly_button_handle_t button_init(shelly_button_cb_t on_press, void *arg)
{
    s_on_press = on_press;
    s_on_press_arg = arg;

    s_press_queue = xQueueCreate(QUEUE_DEPTH, sizeof(uint8_t));
    if (s_press_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create button queue");
        return NULL;
    }
    if (xTaskCreate(button_task, "button", TASK_STACK_SIZE, nullptr, TASK_PRIORITY, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create button task");
        return NULL;
    }

    button_handle_t handle = NULL;
    const button_config_t btn_cfg = {0};

    const button_gpio_config_t btn_gpio_cfg = {
        .gpio_num = BUTTON_GPIO,
        .active_level = BUTTON_ACTIVE_LEVEL,
    };

    if (iot_button_new_gpio_device(&btn_cfg, &btn_gpio_cfg, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create button device");
        return NULL;
    }

    if (iot_button_register_cb(handle, BUTTON_PRESS_DOWN, NULL, button_press_cb, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register button callback");
    }
    ESP_LOGI(TAG, "Button initialized on GPIO%d", BUTTON_GPIO);
    return (shelly_button_handle_t)handle;
}
