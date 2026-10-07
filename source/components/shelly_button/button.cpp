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
#include <esp_timer.h>
#include <button_gpio.h>
#include <iot_button.h>
#include <sdkconfig.h>
#include <status_led.h>

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
#define QUEUE_DEPTH         8
// While a gesture is in progress the task wakes this often to resolve
// taps and to move the LED hint through the hold stages.
#define TICK_MS             50
// The action handler updates Matter attributes, which run the data model
// and reporting on this stack.
#define TASK_STACK_SIZE     4096
#define TASK_PRIORITY       10

typedef struct {
    bool pressed;
    uint32_t at_ms;
} button_edge_t;

static QueueHandle_t s_edge_queue = nullptr;
// Set when an edge could not be queued. The task then drops any gesture in
// progress and resynchronizes with the pin, so a lost release can never be
// read later as a long hold (and a factory reset nobody asked for).
static volatile bool s_edges_lost = false;
static shelly_button_action_cb_t s_on_action = nullptr;
static void *s_on_action_arg = nullptr;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// iot_button callbacks run in the esp_timer task. They only timestamp the
// debounced edge and hand it to the button task, which never blocks the
// timer task on the CHIP stack lock.
static void queue_edge(bool pressed)
{
    button_edge_t edge = {.pressed = pressed, .at_ms = now_ms()};
    if (xQueueSend(s_edge_queue, &edge, 0) != pdTRUE) {
        s_edges_lost = true;
    }
}

static void button_down_cb(void *arg, void *data)
{
    queue_edge(true);
}

static void button_up_cb(void *arg, void *data)
{
    queue_edge(false);
}

static led_hint_t led_hint(button_hint_t hint)
{
    switch (hint) {
    case BUTTON_HINT_WAIT:
        return LED_HINT_HOLD_WAIT;
    case BUTTON_HINT_WIFI:
        return LED_HINT_HOLD_WIFI;
    case BUTTON_HINT_RESET:
        return LED_HINT_HOLD_RESET;
    case BUTTON_HINT_NONE:
    default:
        return LED_HINT_NONE;
    }
}

static const char *action_name(button_action_t action)
{
    switch (action) {
    case BUTTON_ACTION_TOGGLE:
        return "toggle";
    case BUTTON_ACTION_PAIRING:
        return "pairing";
    case BUTTON_ACTION_WIFI_MODE:
        return "wifi mode";
    case BUTTON_ACTION_FACTORY_RESET:
        return "factory reset";
    default:
        return "none";
    }
}

static void button_task(void *arg)
{
    button_logic_t logic;
    button_logic_init(&logic);
    bool pressed = false;
    button_hint_t shown_hint = BUTTON_HINT_NONE;

    while (true) {
        button_edge_t edge;
        TickType_t wait = button_logic_busy(&logic) ? pdMS_TO_TICKS(TICK_MS) : portMAX_DELAY;
        uint32_t at;
        if (xQueueReceive(s_edge_queue, &edge, wait) == pdTRUE) {
            pressed = edge.pressed;
            at = edge.at_ms;
            if (pressed) {
                status_led_flash_press();
            }
        } else {
            at = now_ms();
        }

        if (s_edges_lost) {
            s_edges_lost = false;
            xQueueReset(s_edge_queue);
            button_logic_init(&logic);
            // Start again from the pin's real level. A press already in
            // progress starts its timing now, so it can only come out shorter.
            pressed = gpio_get_level((gpio_num_t)BUTTON_GPIO) == BUTTON_ACTIVE_LEVEL;
            at = now_ms();
            ESP_LOGW(TAG, "Button events were lost; gesture in progress dropped");
        }

        button_action_t action = button_logic_update(&logic, pressed, at);

        button_hint_t hint = button_logic_hint(&logic, now_ms());
        if (hint != shown_hint) {
            shown_hint = hint;
            status_led_set_hint(led_hint(hint));
        }

        if (action != BUTTON_ACTION_NONE) {
            ESP_LOGI(TAG, "Button: %s", action_name(action));
            s_on_action(action, s_on_action_arg);
        }
    }
}

esp_err_t button_init(shelly_button_action_cb_t on_action, void *arg)
{
    s_on_action = on_action;
    s_on_action_arg = arg;

    s_edge_queue = xQueueCreate(QUEUE_DEPTH, sizeof(button_edge_t));
    if (s_edge_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create button queue");
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(button_task, "button", TASK_STACK_SIZE, nullptr, TASK_PRIORITY, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create button task");
        return ESP_ERR_NO_MEM;
    }

    // iot_button only debounces. Gesture timing is button_logic's, so no
    // long-press events or press times are registered here.
    button_handle_t handle = NULL;
    const button_config_t btn_cfg = {0};

    const button_gpio_config_t btn_gpio_cfg = {
        .gpio_num = BUTTON_GPIO,
        .active_level = BUTTON_ACTIVE_LEVEL,
    };

    esp_err_t err = iot_button_new_gpio_device(&btn_cfg, &btn_gpio_cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create button device");
        return err;
    }

    err = iot_button_register_cb(handle, BUTTON_PRESS_DOWN, NULL, button_down_cb, NULL);
    if (err == ESP_OK) {
        err = iot_button_register_cb(handle, BUTTON_PRESS_UP, NULL, button_up_cb, NULL);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register button callbacks");
        return err;
    }
    ESP_LOGI(TAG, "Button initialized on GPIO%d", BUTTON_GPIO);
    return ESP_OK;
}
