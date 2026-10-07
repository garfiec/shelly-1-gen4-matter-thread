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

// thermal.cpp

#include "thermal.h"
#include "thermal_logic.h"

#include <math.h>
#include <stdlib.h>

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_matter.h>
#include <platform/PlatformManager.h>
#include <driver/temperature_sensor.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>

#ifdef CONFIG_SHELLY_HAS_NTC
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>
#endif

#include <relay.h>
#include <shelly_app.h>
#include <shelly_faults.h>

using namespace chip::app::Clusters;
using namespace esp_matter;

static const char *TAG = "thermal";

#define TEMP_SUSTAINED_MS       (CONFIG_SHELLY_THERMAL_SUSTAIN_S * 1000)

// Polling interval for the monitor task.
#define TEMP_POLL_INTERVAL_MS   2000

// Consecutive failed reads before a sensor counts as failed.
#define SENSOR_FAIL_LIMIT       3

// Stack size for the monitor task (sensor drivers, float logging). Matter
// updates run on the Matter thread, not here.
#define THERMAL_TASK_STACK_SIZE 4096

static uint16_t s_relay_endpoint_id = 0;
static uint16_t s_temperature_endpoint_id = 0;

// Die sensor.
static temperature_sensor_handle_t temp_sensor = NULL;
static bool die_installed = false;
static int die_fail_count = 0;
static float die_last_c = NAN;
static thermal_hyst_t die_hyst;

#ifdef CONFIG_SHELLY_HAS_NTC
static adc_oneshot_unit_handle_t ntc_adc = NULL;
static adc_cali_handle_t ntc_cali = NULL;
static adc_channel_t ntc_channel;
static bool ntc_installed = false;
static int ntc_fail_count = 0;
static float ntc_last_c = NAN;
static thermal_hyst_t ntc_hyst;
static const ntc_params_t ntc_params = {
    .r_fixed_ohm = (float)CONFIG_SHELLY_NTC_R_FIXED_OHM,
    .r_nominal_ohm = (float)CONFIG_SHELLY_NTC_R25_OHM,
    .t_nominal_c = 25.0f,
    .beta = (float)CONFIG_SHELLY_NTC_BETA,
    .vref_mv = (float)CONFIG_SHELLY_NTC_VREF_MV,
};
#define NTC_SAMPLES 8
#endif

// Read by the relay gate from other tasks.
static volatile bool overtemp_active = false;
static volatile bool no_sensor = false;

static const char *thermal_gate(void)
{
    if (overtemp_active) {
        return "over temperature";
    }
    if (no_sensor) {
        return "no usable temperature sensor";
    }
    return nullptr;
}

static bool read_die(float *temp)
{
    if (!die_installed) {
        return false;
    }
    if (temperature_sensor_get_celsius(temp_sensor, temp) == ESP_OK) {
        die_fail_count = 0;
        return true;
    }
    if (die_fail_count < SENSOR_FAIL_LIMIT) {
        die_fail_count++;
    }
    ESP_LOGW(TAG, "Die temperature read failed (%d)", die_fail_count);
    return false;
}

#ifdef CONFIG_SHELLY_HAS_NTC
// Returns false if the ADC could not be read at all.
static bool read_ntc(ntc_status_t *status, float *temp)
{
    if (!ntc_installed) {
        return false;
    }
    int sum_mv = 0;
    for (int i = 0; i < NTC_SAMPLES; i++) {
        int raw = 0;
        if (adc_oneshot_read(ntc_adc, ntc_channel, &raw) != ESP_OK) {
            return false;
        }
        int mv = 0;
        if (ntc_cali == NULL || adc_cali_raw_to_voltage(ntc_cali, raw, &mv) != ESP_OK) {
            // Uncalibrated fallback: linear over the 12 dB range.
            mv = raw * CONFIG_SHELLY_NTC_VREF_MV / 4095;
        }
        if (raw >= 4095) {
            // Saturated: the divider is at the high rail.
            mv = CONFIG_SHELLY_NTC_VREF_MV;
        }
        sum_mv += mv;
    }
    *status = ntc_temperature_c(&ntc_params, (float)sum_mv / NTC_SAMPLES, temp);
    return true;
}
#endif

// A null MeasuredValue, as a work argument (outside the int16_t range).
#define TEMPERATURE_NULL_ARG ((intptr_t)INT32_MIN)

// Runs on the Matter thread, so the thermal task never waits on the CHIP
// stack lock: a busy or wedged Matter thread cannot hold up a trip.
static void temperature_work(intptr_t arg)
{
    esp_matter_attr_val_t temp_val = arg == TEMPERATURE_NULL_ARG ? esp_matter_nullable_int16(nullable<int16_t>())
                                                                 : esp_matter_nullable_int16((int16_t)arg);
    attribute::update(s_temperature_endpoint_id, TemperatureMeasurement::Id,
                      TemperatureMeasurement::Attributes::MeasuredValue::Id, &temp_val);
}

static bool schedule_temperature(intptr_t arg)
{
    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(temperature_work, arg);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGW(TAG, "Failed to schedule a temperature report: %" CHIP_ERROR_FORMAT, err.Format());
        return false;
    }
    return true;
}

static void publish_temperature(float temp, bool valid)
{
    // Publish in Matter units (0.01°C), only on a change of 0.5°C or more,
    // so a steady temperature does not generate a Thread report every
    // poll. Skip until Matter has started and the endpoint exists.
    static int16_t last_reported_centi = INT16_MIN;
    static bool reported_null = false;
    if (s_temperature_endpoint_id == 0 || !shelly_app_is_started()) {
        return;
    }
    if (!valid) {
        if (!reported_null && schedule_temperature(TEMPERATURE_NULL_ARG)) {
            reported_null = true;
            last_reported_centi = INT16_MIN;
        }
        return;
    }
    int16_t centi = (int16_t)(temp * 100.0f);
    if ((reported_null || abs(centi - last_reported_centi) >= 50) && schedule_temperature(centi)) {
        reported_null = false;
        last_reported_centi = centi;
    }
}

static void thermal_check(void)
{
    int64_t now_ms = esp_timer_get_time() / 1000;

    // A single failed read keeps the last good value; the sensor only
    // counts as failed after SENSOR_FAIL_LIMIT failures in a row.
    float die_c = NAN;
    if (read_die(&die_c)) {
        die_last_c = die_c;
        thermal_hyst_update(&die_hyst, die_c, now_ms);
    }
    bool die_usable = die_installed && die_fail_count < SENSOR_FAIL_LIMIT && !isnan(die_last_c);

    const thermal_hyst_t *governing = nullptr;
    float temp = NAN;

#ifdef CONFIG_SHELLY_HAS_NTC
    // Like the die sensor, one bad read (ADC error or an open reading)
    // keeps the last good value; the NTC only counts as open after
    // SENSOR_FAIL_LIMIT in a row.
    ntc_status_t ntc_status = NTC_READING_OPEN;
    float ntc_c = NAN;
    if (read_ntc(&ntc_status, &ntc_c) && ntc_status == NTC_READING_OK) {
        ntc_fail_count = 0;
        ntc_last_c = ntc_c;
        thermal_hyst_update(&ntc_hyst, ntc_c, now_ms);
    } else if (ntc_fail_count < SENSOR_FAIL_LIMIT) {
        ntc_fail_count++;
    }
    bool ntc_usable = ntc_installed && ntc_fail_count < SENSOR_FAIL_LIMIT && !isnan(ntc_last_c);
    if (ntc_usable) {
        governing = &ntc_hyst;
        temp = ntc_last_c;
    }
    // Only an open circuit is a sensor fault. A shorted NTC reads as very
    // hot and trips on over-temperature instead.
    shelly_fault_set(SHELLY_FAULT_SRC_NTC_OPEN, !ntc_usable);
#endif

    // The die sensor is the fallback, and the only sensor on boards
    // without an NTC.
    if (governing == nullptr && die_usable) {
        governing = &die_hyst;
        temp = die_last_c;
    }

    bool sensor_missing = (governing == nullptr);
    if (sensor_missing != no_sensor) {
        no_sensor = sensor_missing;
        if (sensor_missing) {
            ESP_LOGE(TAG, "No usable temperature sensor, relay turn-on refused");
        } else {
            ESP_LOGI(TAG, "Temperature sensor recovered");
        }
        shelly_fault_set(SHELLY_FAULT_SRC_TEMP_SENSOR, sensor_missing);
    }

    ESP_LOGD(TAG, "Governing temp: %.1f C", temp);

    // With no sensor the over-temperature state holds; turn-on is refused
    // by the sensor gate anyway.
    if (governing != nullptr) {
        bool trip = governing->tripped;
        if (trip && !overtemp_active) {
            ESP_LOGE(TAG, "Thermal fault, temp: %.1f C, forcing relay off", temp);
            // Set the gate state first, then open the relay under the relay
            // lock, so an in-flight turn-on cannot re-energize it.
            overtemp_active = true;
            shelly_fault_set(SHELLY_FAULT_SRC_OVERTEMP, true);
            relay_trip();
            // Then tell connected ecosystems.
            shelly_onoff_set_async(s_relay_endpoint_id, false);
        } else if (!trip && overtemp_active && temp < governing->clear_c) {
            // The governing sensor must itself read below its clear level:
            // after a switch to the other sensor (an NTC read failing during
            // a trip), that sensor's untripped state alone does not clear it.
            ESP_LOGI(TAG, "Temperature back below %.1f C, turn-on allowed", governing->clear_c);
            overtemp_active = false;
            shelly_fault_set(SHELLY_FAULT_SRC_OVERTEMP, false);
        }
    }

    publish_temperature(temp, governing != nullptr);
}

static void thermal_monitor_task(void *arg)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(TEMP_POLL_INTERVAL_MS));
        thermal_check();
    }
}

#ifdef CONFIG_SHELLY_HAS_NTC
static void ntc_init(void)
{
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(CONFIG_SHELLY_NTC_GPIO, &unit, &ntc_channel) != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d is not an ADC pin", CONFIG_SHELLY_NTC_GPIO);
        return;
    }
    adc_oneshot_unit_init_cfg_t unit_cfg = {};
    unit_cfg.unit_id = unit;
    if (adc_oneshot_new_unit(&unit_cfg, &ntc_adc) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create the NTC ADC unit");
        return;
    }
    adc_oneshot_chan_cfg_t chan_cfg = {};
    chan_cfg.atten = ADC_ATTEN_DB_12;
    chan_cfg.bitwidth = ADC_BITWIDTH_12;
    if (adc_oneshot_config_channel(ntc_adc, ntc_channel, &chan_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure the NTC ADC channel");
        return;
    }
    adc_cali_curve_fitting_config_t cali_cfg = {};
    cali_cfg.unit_id = unit;
    cali_cfg.chan = ntc_channel;
    cali_cfg.atten = ADC_ATTEN_DB_12;
    cali_cfg.bitwidth = ADC_BITWIDTH_12;
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &ntc_cali) != ESP_OK) {
        ESP_LOGW(TAG, "No ADC calibration, NTC readings are approximate");
        ntc_cali = NULL;
    }
    thermal_hyst_init(&ntc_hyst, CONFIG_SHELLY_THERMAL_NTC_TRIP_C, CONFIG_SHELLY_THERMAL_NTC_CLEAR_C,
                      TEMP_SUSTAINED_MS);
    ntc_installed = true;
    ESP_LOGI(TAG, "NTC on GPIO%d (trip %d C, clear %d C)", CONFIG_SHELLY_NTC_GPIO,
             CONFIG_SHELLY_THERMAL_NTC_TRIP_C, CONFIG_SHELLY_THERMAL_NTC_CLEAR_C);
}
#endif

esp_err_t thermal_init(void)
{
    thermal_hyst_init(&die_hyst, CONFIG_SHELLY_THERMAL_DIE_TRIP_C, CONFIG_SHELLY_THERMAL_DIE_CLEAR_C,
                      TEMP_SUSTAINED_MS);

    temperature_sensor_config_t temp_cfg = {
        .range_min = 20,
        .range_max = 100,
    };
    esp_err_t err = temperature_sensor_install(&temp_cfg, &temp_sensor);
    if (err == ESP_OK) {
        err = temperature_sensor_enable(temp_sensor);
    }
    if (err == ESP_OK) {
        die_installed = true;
    } else {
        ESP_LOGE(TAG, "Die temperature sensor unavailable: %s", esp_err_to_name(err));
    }

#ifdef CONFIG_SHELLY_HAS_NTC
    ntc_init();
#endif

    esp_err_t gate_err = relay_register_gate(thermal_gate);

    // First check now, so the gates hold real state before anything can
    // turn the relay on.
    thermal_check();

    if (xTaskCreate(thermal_monitor_task, "thermal_monitor",
                    THERMAL_TASK_STACK_SIZE, NULL, 5, NULL) != pdPASS) {
        // Without the monitor nothing watches the temperature. Refuse
        // turn-on rather than run unprotected.
        ESP_LOGE(TAG, "Failed to create thermal task, relay turn-on refused");
        no_sensor = true;
        shelly_fault_set(SHELLY_FAULT_SRC_TEMP_SENSOR, true);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Thermal monitor initialized (die trip %d C, clear %d C, sustained %dms)",
             CONFIG_SHELLY_THERMAL_DIE_TRIP_C, CONFIG_SHELLY_THERMAL_DIE_CLEAR_C, TEMP_SUSTAINED_MS);
    return gate_err;
}

void thermal_set_endpoints(uint16_t relay_endpoint_id, uint16_t temperature_endpoint_id)
{
    s_relay_endpoint_id = relay_endpoint_id;
    s_temperature_endpoint_id = temperature_endpoint_id;
}
