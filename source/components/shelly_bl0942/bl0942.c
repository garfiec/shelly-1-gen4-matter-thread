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

// bl0942.c

#include <sdkconfig.h>

#ifdef CONFIG_SHELLY_HAS_BL0942

#include "bl0942.h"

#include <inttypes.h>
#include <stdlib.h>

#include <esp_log.h>
#include <driver/gpio.h>
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "bl0942";

#define BL0942_UART         ((uart_port_t)CONFIG_SHELLY_BL0942_UART_NUM)
#define BL0942_BAUD         9600
#define RX_BUF_SIZE         256
// 23 bytes at 9600 baud take 24ms; allow for the chip's response delay.
#define FRAME_TIMEOUT_MS    100
#define REG_TIMEOUT_MS      50

static bl0942_cal_t s_cal;
static bool s_uart_ready = false;

static esp_err_t write_reg(uint8_t reg, uint32_t value)
{
    uint8_t pkt[6];
    bl0942_build_write(reg, value, pkt);
    if (uart_write_bytes(BL0942_UART, pkt, sizeof(pkt)) != (int)sizeof(pkt)) {
        return ESP_FAIL;
    }
    esp_err_t err = uart_wait_tx_done(BL0942_UART, pdMS_TO_TICKS(REG_TIMEOUT_MS));
    // The chip needs a moment between commands.
    vTaskDelay(pdMS_TO_TICKS(2));
    return err;
}

static esp_err_t read_reg(uint8_t reg, uint32_t *value)
{
    uint8_t cmd[2] = {BL0942_CMD_READ, reg};
    uint8_t resp[4];
    uart_flush_input(BL0942_UART);
    if (uart_write_bytes(BL0942_UART, cmd, sizeof(cmd)) != (int)sizeof(cmd)) {
        return ESP_FAIL;
    }
    int n = uart_read_bytes(BL0942_UART, resp, sizeof(resp), pdMS_TO_TICKS(REG_TIMEOUT_MS));
    if (n != (int)sizeof(resp)) {
        return ESP_ERR_TIMEOUT;
    }
    return bl0942_parse_reg_response(reg, resp, value) ? ESP_OK : ESP_ERR_INVALID_CRC;
}

esp_err_t bl0942_set_line_frequency(bool line_60hz)
{
    if (!s_uart_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t mode = bl0942_mode_value(line_60hz);
    esp_err_t err = write_reg(BL0942_REG_USR_WRPROT, BL0942_USR_WRPROT_UNLOCK);
    if (err == ESP_OK) {
        err = write_reg(BL0942_REG_MODE, mode);
    }
    // Always re-lock, even after a failed write.
    write_reg(BL0942_REG_USR_WRPROT, 0);
    if (err != ESP_OK) {
        return err;
    }

    uint32_t readback = 0;
    err = read_reg(BL0942_REG_MODE, &readback);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MODE readback failed: %s", esp_err_to_name(err));
        return err;
    }
    if (readback != mode) {
        ESP_LOGW(TAG, "MODE readback 0x%06" PRIx32 ", expected 0x%06" PRIx32, readback, mode);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "MODE 0x%06" PRIx32 " (%d Hz)", mode, line_60hz ? 60 : 50);
    return ESP_OK;
}

esp_err_t bl0942_init(bool line_60hz)
{
    bl0942_cal_init(&s_cal, strtof(CONFIG_SHELLY_BL0942_VOLTAGE_REF, NULL),
                    strtof(CONFIG_SHELLY_BL0942_CURRENT_REF, NULL),
                    (float)CONFIG_SHELLY_BL0942_CURRENT_FULL_SCALE_MA / 1000.0f);

    const uart_config_t cfg = {
        .baud_rate = BL0942_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(BL0942_UART, RX_BUF_SIZE, 0, 0, NULL, 0);
    if (err == ESP_OK) {
        err = uart_param_config(BL0942_UART, &cfg);
    }
    if (err == ESP_OK) {
        err = uart_set_pin(BL0942_UART, CONFIG_SHELLY_BL0942_TX_GPIO, CONFIG_SHELLY_BL0942_RX_GPIO,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART setup failed: %s", esp_err_to_name(err));
        return err;
    }
    // Keep RX defined while the chip is quiet.
    gpio_pullup_en(CONFIG_SHELLY_BL0942_RX_GPIO);
    s_uart_ready = true;

    ESP_LOGI(TAG, "UART%d TX GPIO%d RX GPIO%d, %.1f pulses/kWh", CONFIG_SHELLY_BL0942_UART_NUM,
             CONFIG_SHELLY_BL0942_TX_GPIO, CONFIG_SHELLY_BL0942_RX_GPIO, s_cal.pulses_per_kwh);
    return bl0942_set_line_frequency(line_60hz);
}

esp_err_t bl0942_read(bl0942_raw_t *raw)
{
    if (!s_uart_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    static const uint8_t cmd[2] = {BL0942_CMD_READ, BL0942_FULL_PACKET};
    uint8_t buf[BL0942_FRAME_LEN];

    uart_flush_input(BL0942_UART);
    if (uart_write_bytes(BL0942_UART, cmd, sizeof(cmd)) != (int)sizeof(cmd)) {
        return ESP_FAIL;
    }
    int n = uart_read_bytes(BL0942_UART, buf, sizeof(buf), pdMS_TO_TICKS(FRAME_TIMEOUT_MS));
    if (n < 0) {
        return ESP_FAIL;
    }
    switch (bl0942_parse_frame(buf, (size_t)n, raw)) {
    case BL0942_FRAME_OK:
        return ESP_OK;
    case BL0942_FRAME_SHORT:
        return ESP_ERR_TIMEOUT;
    case BL0942_FRAME_BAD_CHECKSUM:
        return ESP_ERR_INVALID_CRC;
    default:
        return ESP_ERR_INVALID_RESPONSE;
    }
}

const bl0942_cal_t *bl0942_calibration(void)
{
    return &s_cal;
}

#endif // CONFIG_SHELLY_HAS_BL0942
