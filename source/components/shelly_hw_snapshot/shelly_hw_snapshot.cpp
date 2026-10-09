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


// shelly_hw_snapshot.cpp

#include "shelly_hw_snapshot.h"

#include <math.h>

#include <driver/gpio.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <relay.h>
#include <switch_input.h>
#include <thermal.h>

static const char *TAG = "hw";

#define SAMPLE_PERIOD_S     10
#define LOG_EVERY_SAMPLES   6       // One log line a minute
#define TASK_STACK_SIZE     4096
#define TASK_PRIORITY       1       // Lowest useful: never delays the inputs or Matter

// Survives a software restart (into Wi-Fi mode and back); garbage after a
// power cut, which hw_snap_valid() rejects.
static RTC_NOINIT_ATTR hw_snap_t s_snap;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
// Set once a normal boot has started its snapshot: until then (and in
// Wi-Fi mode) the snapshot is the last boot's and nothing may change it.
static volatile bool s_active = false;

static int16_t to_c10(float c)
{
    if (isnan(c) || c < -3000.0f || c > 3000.0f) {
        return HW_SNAP_NO_TEMP;
    }
    return (int16_t)lroundf(c * 10.0f);
}

static uint8_t pins_high(bool (*level)(int), int count)
{
    uint8_t bits = 0;
    for (int ch = 0; ch < count; ch++) {
        if (level(ch)) {
            bits |= (uint8_t)(1u << ch);
        }
    }
    return bits;
}

static uint8_t button_pin(void)
{
    return gpio_get_level((gpio_num_t)CONFIG_SHELLY_BUTTON_GPIO) ? 1 : 0;
}

static bool crashed(esp_reset_reason_t reason)
{
    return reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT ||
           reason == ESP_RST_WDT;
}

static void sample(void)
{
    thermal_readings_t t;
    thermal_get_readings(&t);
    uint32_t free_b = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    uint32_t min_b = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
    uint32_t big_b = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    uint32_t cmd = relay_get_mask();
    uint32_t pin = relay_pin_mask();
    uint8_t sw_raw = pins_high(switch_input_get_level_ch, SWITCH_INPUT_CHANNELS);
    uint8_t sw_stable = pins_high(switch_input_stable_level_ch, SWITCH_INPUT_CHANNELS);
    uint8_t btn = button_pin();

    taskENTER_CRITICAL(&s_mux);
    s_snap.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    s_snap.heap_free = free_b;
    s_snap.heap_min = min_b;
    s_snap.heap_big = big_b;
    s_snap.relay_cmd = cmd;
    s_snap.relay_pin = pin;
    s_snap.sw_raw = sw_raw;
    s_snap.sw_stable = sw_stable;
    for (int ch = 0; ch < SWITCH_INPUT_CHANNELS && ch < HW_SNAP_CHANNELS; ch++) {
        s_snap.sw_edges[ch] = switch_input_edge_count(ch);
    }
    s_snap.button_raw = btn;
    s_snap.ntc_raw = (int16_t)t.ntc_raw;
    s_snap.ntc_mv = (int16_t)t.ntc_mv;
    s_snap.ntc_c10 = to_c10(t.ntc_c);
    s_snap.die_c10 = to_c10(t.die_c);
    s_snap.ntc_ok = t.ntc_ok;
    s_snap.ntc_status = (uint8_t)t.ntc_status;
    s_snap.governing_ntc = t.governing_ntc;
    s_snap.overtemp = t.overtemp;
    s_snap.no_sensor = t.no_sensor;
    hw_snap_seal(&s_snap);
    taskEXIT_CRITICAL(&s_mux);
}

static void snapshot_task(void *arg)
{
    for (int n = 0;; n++) {
        sample();
        if (n % LOG_EVERY_SAMPLES == 0) {
            // Static: only this task uses it, and it keeps 400 bytes off
            // the stack the log hook also uses.
            static hw_snap_t copy;
            taskENTER_CRITICAL(&s_mux);
            copy = s_snap;
            taskEXIT_CRITICAL(&s_mux);
            char line[96];
            hw_snap_line(&copy, line, sizeof(line));
            ESP_LOGI(TAG, "%s", line);
        }
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_S * 1000));
    }
}

void shelly_hw_snapshot_start(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    bool kept = reason != ESP_RST_POWERON && hw_snap_valid(&s_snap);
    if (crashed(reason)) {
        // The last boot crashed. Whatever caused it, this boot doesn't
        // sample, so the snapshot can never cause a crash loop, and the
        // crashed boot's snapshot is kept for the page.
        taskENTER_CRITICAL(&s_mux);
        if (!kept) {
            hw_snap_reset(&s_snap);
            s_snap.channels = RELAY_CHANNELS;
        }
        s_snap.boots = kept ? s_snap.boots + 1 : 1;
        s_snap.reset_reason = (uint32_t)reason;
        hw_snap_seal(&s_snap);
        taskEXIT_CRITICAL(&s_mux);
        ESP_LOGW(TAG, "Hardware snapshot skipped this boot: the last boot crashed (reset reason %d)", (int)reason);
        return;
    }
    uint32_t boots = kept ? s_snap.boots + 1 : 1;

    taskENTER_CRITICAL(&s_mux);
    hw_snap_reset(&s_snap);
    s_snap.boots = boots;
    s_snap.reset_reason = (uint32_t)reason;
    s_snap.channels = RELAY_CHANNELS;
    s_snap.ade_state = HW_ADE_NOT_RUN;
    hw_snap_seal(&s_snap);
    taskEXIT_CRITICAL(&s_mux);
    s_active = true;

    ESP_LOGI(TAG, "Hardware snapshot on (boot %lu, reset reason %d)", (unsigned long)boots, (int)reason);
    if (xTaskCreate(snapshot_task, "hw_snap", TASK_STACK_SIZE, nullptr, TASK_PRIORITY, nullptr) != pdPASS) {
        ESP_LOGW(TAG, "Hardware snapshot task not started");
    }
}

bool shelly_hw_snapshot_active(void)
{
    return s_active;
}

void shelly_hw_snapshot_ade_state(hw_ade_state_t state, int32_t err)
{
    if (!s_active) {
        return;
    }
    taskENTER_CRITICAL(&s_mux);
    s_snap.ade_state = (uint8_t)state;
    s_snap.ade_err = err;
    hw_snap_seal(&s_snap);
    taskEXIT_CRITICAL(&s_mux);
}

void shelly_hw_snapshot_ade_reg(uint16_t addr, uint8_t width, bool ok, uint32_t value)
{
    if (!s_active) {
        return;
    }
    taskENTER_CRITICAL(&s_mux);
    hw_snap_set_reg(&s_snap, addr, width, ok, value);
    hw_snap_seal(&s_snap);
    taskEXIT_CRITICAL(&s_mux);
}

void shelly_hw_snapshot_ade_cycle_done(bool dump)
{
    if (!s_active) {
        return;
    }
    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);
    taskENTER_CRITICAL(&s_mux);
    if (dump) {
        s_snap.ade_dump_s = now_s;
    } else {
        s_snap.ade_live_reads++;
    }
    s_snap.ade_live_s = now_s;
    hw_snap_seal(&s_snap);
    taskEXIT_CRITICAL(&s_mux);
}

bool shelly_hw_snapshot_ade_get(uint16_t addr, uint32_t *value)
{
    taskENTER_CRITICAL(&s_mux);
    bool ok = hw_snap_get_reg(&s_snap, addr, value);
    taskEXIT_CRITICAL(&s_mux);
    return ok;
}

size_t shelly_hw_snapshot_json(char *buf, size_t len)
{
    // Wi-Fi mode: nothing writes the snapshot in this boot.
    thermal_readings_t t;
    thermal_get_readings(&t);
    hw_live_t live = {};
    live.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    live.heap_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    live.heap_min = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
    live.relay_cmd = relay_get_mask();
    live.relay_pin = relay_pin_mask();
    live.channels = RELAY_CHANNELS;
    live.sw_raw = pins_high(switch_input_get_level_ch, SWITCH_INPUT_CHANNELS);
    live.button_raw = button_pin();
    live.ntc_raw = (int16_t)t.ntc_raw;
    live.ntc_mv = (int16_t)t.ntc_mv;
    live.ntc_c10 = to_c10(t.ntc_c);
    live.die_c10 = to_c10(t.die_c);
    return hw_snap_json(&s_snap, &live, buf, len);
}
