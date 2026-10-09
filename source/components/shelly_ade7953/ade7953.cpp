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


// ade7953.cpp

#include "ade7953.h"
#include "ade7953_regs.h"

#include <stdlib.h>

#include <driver/i2c_master.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <sdkconfig.h>

#if CONFIG_SHELLY_HW_SNAPSHOT
#include <shelly_hw_snapshot.h>
#endif

static const char *TAG = "ade7953";

#define I2C_TIMEOUT_MS          50
#define SETUP_CHECK_US          (10LL * 1000 * 1000)
#define SNAPSHOT_PERIOD_US      (60LL * 1000 * 1000)

static i2c_master_bus_handle_t s_bus = nullptr;
static i2c_master_dev_handle_t s_dev = nullptr;
static ade7953_cal_t s_cal;
static int64_t s_setup_checked_us = 0;
static bool s_setup_ok = false;
#if CONFIG_SHELLY_HW_SNAPSHOT
static int64_t s_snapshot_us = 0;
static bool s_snapshot_dumped = false;
#endif

// Read one register: the 2-byte address, then (repeated start) its bytes,
// most significant first.
static esp_err_t read_reg(uint16_t addr, uint32_t *value)
{
    int width = ade7953_reg_width(addr);
    if (s_dev == nullptr || width == 0 || ade7953_reg_read_resets(addr)) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t a[2] = {(uint8_t)(addr >> 8), (uint8_t)addr};
    uint8_t d[4] = {};
    esp_err_t err = i2c_master_transmit_receive(s_dev, a, sizeof(a), d, width / 8, I2C_TIMEOUT_MS);
    if (err == ESP_OK) {
        *value = ade7953_be_value(d, width / 8);
    }
    return err;
}

// The only write path: the address, then the value's bytes. Refuses
// anything but the setup (ade7953_write_allowed()).
static esp_err_t write_reg(uint16_t addr, uint32_t value)
{
    int width = ade7953_reg_width(addr);
    if (s_dev == nullptr || width == 0 || !ade7953_write_allowed(addr, value)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t b[6] = {(uint8_t)(addr >> 8), (uint8_t)addr};
    int n = width / 8;
    for (int i = 0; i < n; i++) {
        b[2 + i] = (uint8_t)(value >> (8 * (n - 1 - i)));
    }
    return i2c_master_transmit(s_dev, b, 2 + n, I2C_TIMEOUT_MS);
}

// Write the setup if the chip does not hold it (after its power-on or a
// brownout), and confirm it by reading it back.
static void check_setup(void)
{
    uint32_t v = 0;
    if (read_reg(ADE7953_REG_SETUP, &v) == ESP_OK && v == ADE7953_SETUP_VALUE) {
        s_setup_ok = true;
        return;
    }
    if (s_setup_ok) {
        ESP_LOGW(TAG, "The meter lost its setup (reset?): writing it again");
    }
    s_setup_ok = false;
    if (write_reg(ADE7953_REG_UNLOCK, ADE7953_UNLOCK_VALUE) == ESP_OK &&
        write_reg(ADE7953_REG_SETUP, ADE7953_SETUP_VALUE) == ESP_OK && read_reg(ADE7953_REG_SETUP, &v) == ESP_OK &&
        v == ADE7953_SETUP_VALUE) {
        s_setup_ok = true;
        ESP_LOGI(TAG, "Meter set up");
    }
}

#if CONFIG_SHELLY_HW_SNAPSHOT
// A normal boot's hardware snapshot: the whole register table once, then
// the measurements every minute. Nothing in Wi-Fi mode, where the snapshot
// is the last normal boot's.
static void snapshot_feed(int64_t now_us)
{
    if (!shelly_hw_snapshot_active() || (s_snapshot_dumped && now_us - s_snapshot_us < SNAPSHOT_PERIOD_US)) {
        return;
    }
    s_snapshot_us = now_us;
    esp_err_t last = ESP_OK;
    for (int i = 0; i < ADE7953_PROBE_REG_COUNT; i++) {
        const ade7953_reg_t *r = &ADE7953_PROBE_REGS[i];
        if (s_snapshot_dumped && !r->live) {
            continue;
        }
        uint32_t v = 0;
        esp_err_t err = read_reg(r->addr, &v);
        if (err != ESP_OK) {
            last = err;
        }
        shelly_hw_snapshot_ade_reg(r->addr, (uint8_t)ade7953_reg_width(r->addr), err == ESP_OK, v);
    }
    shelly_hw_snapshot_ade_cycle_done(!s_snapshot_dumped);
    shelly_hw_snapshot_ade_state(last == ESP_OK ? HW_ADE_DONE : HW_ADE_READ_ERRORS, last);
    s_snapshot_dumped = true;
}
#endif

esp_err_t ade7953_read(ade7953_raw_t *raw)
{
    int64_t now_us = esp_timer_get_time();
    if (now_us - s_setup_checked_us >= SETUP_CHECK_US) {
        s_setup_checked_us = now_us;
        check_setup();
    }

    uint32_t v = 0, ia = 0, ib = 0, pa = 0, pb = 0, per = 0;
    esp_err_t err = read_reg(ADE7953_REG_VRMS, &v);
    if (err == ESP_OK) {
        err = read_reg(ADE7953_REG_IRMSA, &ia);
    }
    if (err == ESP_OK) {
        err = read_reg(ADE7953_REG_IRMSB, &ib);
    }
    if (err == ESP_OK) {
        err = read_reg(ADE7953_REG_AWATT, &pa);
    }
    if (err == ESP_OK) {
        err = read_reg(ADE7953_REG_BWATT, &pb);
    }
    if (err == ESP_OK) {
        err = read_reg(ADE7953_REG_PERIOD, &per);
    }
    if (err != ESP_OK) {
        return err;
    }
    raw->vrms = v;
    raw->irms[0] = ia;
    raw->irms[1] = ib;
    raw->watt[0] = (int32_t)pa;
    raw->watt[1] = (int32_t)pb;
    raw->period = (uint16_t)per;

#if CONFIG_SHELLY_HW_SNAPSHOT
    snapshot_feed(now_us);
#endif
    return ESP_OK;
}

const ade7953_cal_t *ade7953_calibration(void)
{
    return &s_cal;
}

esp_err_t ade7953_init(void)
{
    s_cal.v_mul = strtof(CONFIG_SHELLY_ADE7953_V_MUL, nullptr);
    s_cal.i_mul[0] = strtof(CONFIG_SHELLY_ADE7953_IA_MUL, nullptr);
    s_cal.i_mul[1] = strtof(CONFIG_SHELLY_ADE7953_IB_MUL, nullptr);
    s_cal.p_mul[0] = strtof(CONFIG_SHELLY_ADE7953_PA_MUL, nullptr);
    s_cal.p_mul[1] = strtof(CONFIG_SHELLY_ADE7953_PB_MUL, nullptr);

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
        ESP_LOGE(TAG, "I2C bus setup failed: %s", esp_err_to_name(err));
        return err;
    }
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = ADE7953_I2C_ADDR;
    dev_cfg.scl_speed_hz = CONFIG_SHELLY_ADE7953_I2C_HZ;
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        s_dev = nullptr;
        ESP_LOGE(TAG, "I2C device setup failed: %s", esp_err_to_name(err));
        return err;
    }

    uint32_t version = 0;
    err = read_reg(0x702, &version);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No answer from the meter at 0x%02X: %s", ADE7953_I2C_ADDR, esp_err_to_name(err));
        return err;
    }
    s_setup_checked_us = esp_timer_get_time();
    check_setup();
    ESP_LOGI(TAG, "ADE7953 version 0x%02lX, setup %s", (unsigned long)version, s_setup_ok ? "ok" : "FAILED");
    return s_setup_ok ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
