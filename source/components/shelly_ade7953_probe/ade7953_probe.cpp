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


// ade7953_probe.cpp

#include "ade7953_probe.h"
#include "ade7953_regs.h"

#include <driver/i2c_master.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <shelly_hw_snapshot.h>

static const char *TAG = "ade";

#define I2C_TIMEOUT_MS      50
#define MAX_FAILS_IN_ROW    3
#define LIVE_PERIOD_S       60
#define LOG_LIVE_EVERY      5       // A live line in the log every 5 minutes
#define TASK_STACK_SIZE     3584
#define TASK_PRIORITY       2

static i2c_master_bus_handle_t s_bus = nullptr;
static i2c_master_dev_handle_t s_dev = nullptr;

// Read one register: the 2-byte address, then (repeated start) its bytes,
// most significant first. A read only: the chip changes a register only
// when data bytes follow the address in a write, and nothing here sends any.
static esp_err_t read_reg(uint16_t addr, int width, uint32_t *value)
{
    uint8_t a[2] = {(uint8_t)(addr >> 8), (uint8_t)addr};
    uint8_t d[4] = {};
    int n = width / 8;
    esp_err_t err = i2c_master_transmit_receive(s_dev, a, sizeof(a), d, n, I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        v = (v << 8) | d[i];
    }
    *value = v;
    return ESP_OK;
}

// One pass over the table (live only: the per-minute subset). False once
// reads failed MAX_FAILS_IN_ROW times in a row.
static bool read_pass(bool live_only, bool log_each, esp_err_t *last_err)
{
    int fails = 0;
    for (int i = 0; i < ADE7953_PROBE_REG_COUNT; i++) {
        const ade7953_reg_t *r = &ADE7953_PROBE_REGS[i];
        if (live_only && !r->live) {
            continue;
        }
        int width = ade7953_reg_width(r->addr);
        if (width == 0 || ade7953_reg_read_resets(r->addr)) {
            // The host tests keep the table clean; this only guards an edit.
            continue;
        }
        uint32_t v = 0;
        esp_err_t err = read_reg(r->addr, width, &v);
        shelly_hw_snapshot_ade_reg(r->addr, (uint8_t)width, err == ESP_OK, v);
        if (err != ESP_OK) {
            *last_err = err;
            ESP_LOGW(TAG, "0x%03X %s read failed: %s", r->addr, r->name, esp_err_to_name(err));
            if (++fails >= MAX_FAILS_IN_ROW) {
                return false;
            }
            continue;
        }
        fails = 0;
        if (log_each) {
            ESP_LOGI(TAG, "0x%03X/%d %s = 0x%0*lX", r->addr, width, r->name, width / 4, (unsigned long)v);
        }
    }
    return true;
}

static void log_live(void)
{
    uint32_t v = 0, ia = 0, ib = 0, pa = 0, pb = 0, per = 0;
    shelly_hw_snapshot_ade_get(0x31C, &v);
    shelly_hw_snapshot_ade_get(0x31A, &ia);
    shelly_hw_snapshot_ade_get(0x31B, &ib);
    shelly_hw_snapshot_ade_get(0x312, &pa);
    shelly_hw_snapshot_ade_get(0x313, &pb);
    shelly_hw_snapshot_ade_get(0x10E, &per);
    ESP_LOGI(TAG, "V %lx IA %lx IB %lx PA %lx PB %lx per %lu", (unsigned long)v, (unsigned long)ia,
             (unsigned long)ib, (unsigned long)pa, (unsigned long)pb, (unsigned long)per);
}

static void finish(hw_ade_state_t state, esp_err_t err)
{
    shelly_hw_snapshot_ade_guard(false);
    shelly_hw_snapshot_ade_state(state, err);
    if (s_dev != nullptr) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = nullptr;
    }
    if (s_bus != nullptr) {
        i2c_del_master_bus(s_bus);
        s_bus = nullptr;
    }
    ESP_LOGI(TAG, "Meter probe stopped: %s", hw_ade_state_name(state));
}

static void probe_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(CONFIG_SHELLY_ADE7953_PROBE_DELAY_S * 1000));
    ESP_LOGI(TAG, "Meter probe: reading the ADE7953 at 0x%02X (read only)", ADE7953_I2C_ADDR);
    shelly_hw_snapshot_ade_state(HW_ADE_RUNNING, 0);
    shelly_hw_snapshot_ade_guard(true);

    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = -1; // Any free port
    bus_cfg.sda_io_num = (gpio_num_t)CONFIG_SHELLY_ADE7953_SDA_GPIO;
    bus_cfg.scl_io_num = (gpio_num_t)CONFIG_SHELLY_ADE7953_SCL_GPIO;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        s_bus = nullptr;
        finish(HW_ADE_BUS_ERROR, err);
        vTaskDelete(nullptr);
        return;
    }
    err = i2c_master_probe(s_bus, ADE7953_I2C_ADDR, I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No answer at 0x%02X: %s", ADE7953_I2C_ADDR, esp_err_to_name(err));
        finish(HW_ADE_NO_ANSWER, err);
        vTaskDelete(nullptr);
        return;
    }
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = ADE7953_I2C_ADDR;
    dev_cfg.scl_speed_hz = CONFIG_SHELLY_ADE7953_I2C_HZ;
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        s_dev = nullptr;
        finish(HW_ADE_BUS_ERROR, err);
        vTaskDelete(nullptr);
        return;
    }

    esp_err_t last_err = ESP_OK;
    if (!read_pass(false, true, &last_err)) {
        finish(HW_ADE_READ_ERRORS, last_err);
        vTaskDelete(nullptr);
        return;
    }
    shelly_hw_snapshot_ade_cycle_done(true);
    shelly_hw_snapshot_ade_state(HW_ADE_DONE, last_err);
    shelly_hw_snapshot_ade_guard(false);
    log_live();

    for (int n = 1;; n++) {
        vTaskDelay(pdMS_TO_TICKS(LIVE_PERIOD_S * 1000));
        shelly_hw_snapshot_ade_guard(true);
        bool ok = read_pass(true, false, &last_err);
        shelly_hw_snapshot_ade_guard(false);
        if (!ok) {
            finish(HW_ADE_READ_ERRORS, last_err);
            vTaskDelete(nullptr);
            return;
        }
        shelly_hw_snapshot_ade_cycle_done(false);
        if (n % LOG_LIVE_EVERY == 0) {
            log_live();
        }
    }
}

void ade7953_probe_start(void)
{
    if (shelly_hw_snapshot_ade_blocked()) {
        ESP_LOGW(TAG, "Meter probe skipped: the device crashed while probing since power-on");
        return;
    }
    if (xTaskCreate(probe_task, "ade_probe", TASK_STACK_SIZE, nullptr, TASK_PRIORITY, nullptr) != pdPASS) {
        ESP_LOGW(TAG, "Meter probe task not started");
    }
}
