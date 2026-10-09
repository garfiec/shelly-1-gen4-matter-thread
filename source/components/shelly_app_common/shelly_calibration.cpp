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


// shelly_calibration.cpp

#include "shelly_calibration.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <nvs.h>

static const char *TAG = "calibration";

// Beside the energy totals (metering.cpp), which a factory reset keeps too.
#define NVS_NAMESPACE   "shelly_meter"
#define NVS_KEY         "cal"

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static cal_ratios_t s_ratios = {{1.0f, 1.0f, 1.0f, 1.0f, 1.0f}};
static float s_base[CAL_Q_COUNT];
static volatile uint32_t s_generation = 0;
static volatile bool s_available = false;

static void log_ratios(const char *what, const cal_ratios_t *r)
{
    ESP_LOGI(TAG, "%s: V x%.4f, I x%.4f / x%.4f, P x%.4f / x%.4f", what, r->q[CAL_Q_V], r->q[CAL_Q_I0],
             r->q[CAL_Q_I1], r->q[CAL_Q_P0], r->q[CAL_Q_P1]);
}

void shelly_cal_init(const float base[CAL_Q_COUNT])
{
    for (int i = 0; i < CAL_Q_COUNT; i++) {
        s_base[i] = base[i];
    }
    cal_ratios_t ratios;
    cal_ratios_default(&ratios);

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        cal_record_t rec;
        size_t len = sizeof(rec);
        esp_err_t err = nvs_get_blob(h, NVS_KEY, &rec, &len);
        nvs_close(h);
        if (err == ESP_OK && len == sizeof(rec) && cal_record_valid(&rec)) {
            cal_ratios_t stored;
            for (int i = 0; i < CAL_Q_COUNT; i++) {
                stored.q[i] = rec.ratio[i];
            }
            if (!cal_rebase(&stored, rec.base, s_base, &ratios)) {
                ESP_LOGW(TAG, "Part of the saved calibration no longer fits this firmware's: reset to built-in");
            }
        } else if (err == ESP_OK) {
            ESP_LOGW(TAG, "Saved calibration unreadable: using the built-in one");
        }
    }

    portENTER_CRITICAL(&s_mux);
    s_ratios = ratios;
    s_generation++;
    portEXIT_CRITICAL(&s_mux);
    s_available = true;
    if (!cal_ratios_is_default(&ratios)) {
        log_ratios("Calibration", &ratios);
    }
}

bool shelly_cal_available(void)
{
    return s_available;
}

void shelly_cal_get(cal_ratios_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_ratios;
    portEXIT_CRITICAL(&s_mux);
}

uint32_t shelly_cal_generation(void)
{
    return s_generation;
}

bool shelly_cal_save(const cal_ratios_t *ratios)
{
    if (!s_available || !cal_ratios_valid(ratios)) {
        return false;
    }
    cal_record_t rec;
    cal_record_make(&rec, s_base, ratios);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY, &rec, sizeof(rec));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Saving the calibration failed: %s", esp_err_to_name(err));
        return false;
    }
    portENTER_CRITICAL(&s_mux);
    s_ratios = *ratios;
    s_generation++;
    portEXIT_CRITICAL(&s_mux);
    log_ratios("Calibration saved", ratios);
    return true;
}
