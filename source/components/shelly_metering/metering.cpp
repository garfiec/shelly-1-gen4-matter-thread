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

// metering.cpp

#include <sdkconfig.h>

#ifdef CONFIG_SHELLY_HAS_BL0942

#include "metering.h"
#include "metering_logic.h"

#include <math.h>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>

#include <app/clusters/electrical-power-measurement-server/ElectricalPowerMeasurementCluster.h>
#include <app/server-cluster/ServerClusterInterfaceRegistry.h>
#include <data_model_provider/clusters/electrical_energy_measurement/integration.h>
#include <data_model_provider/esp_matter_data_model_provider.h>
#include <platform/PlatformManager.h>
#include <system/SystemClock.h>

#include <bl0942.h>
#include <protection.h>
#include <protection_logic.h>
#include <relay.h>

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters;
using namespace esp_matter;

namespace EPM = chip::app::Clusters::ElectricalPowerMeasurement;

static const char *TAG = "metering";

#define POLL_MS                 200
#define PUBLISH_MS              1000
#define HEARTBEAT_MS            60000
#define METER_STALE_MS          (CONFIG_SHELLY_PROT_METER_LOST_S * 1000LL)
#define TASK_STACK_SIZE         5120
#define TASK_PRIORITY           8

// AC detection at boot: V_RMS >= 80 V on 3 consecutive samples, 400ms
// apart, within 3s. Three samples span more than one 800ms RMS update
// (MODE), so at least two different updates agree.
#define AC_DETECT_V             80.0f
#define AC_DETECT_COUNT         3
#define AC_DETECT_SAMPLE_MS     400
#define AC_DETECT_WINDOW_MS     3000

// Energy: glitch cap and persistence.
#define MAX_PLAUSIBLE_W         5000.0f
#define SAVE_DELTA_MWH          (100LL * 1000)          // 100 Wh
#define SAVE_INTERVAL_MS        (15LL * 60 * 1000)      // 15 min
// Allowance on the first delta after a software reset: the energy counted
// since the last save (under SAVE_DELTA_MWH), plus what the meter kept
// counting while the ESP32 restarted (up to a minute at MAX_PLAUSIBLE_W).
#define RESTORE_SLACK_MWH       (SAVE_DELTA_MWH + (int64_t)(MAX_PLAUSIBLE_W * 60.0f / 3.6f))
#define NOTIFY_DELTA_MWH        (10LL * 1000)           // 10 Wh
#define NOTIFY_INTERVAL_MS      (300LL * 1000)          // 5 min

// Report thresholds.
#define POWER_STEP_W            1.0f
#define POWER_STEP_REL          0.02f
#define VOLTAGE_STEP_V          1.0f
#define CURRENT_STEP_A          0.05f
#define FREQ_STEP_HZ            0.1f

// Stored energy and the AC latch survive a factory reset: they describe
// the hardware and its history, not the Matter commissioning.
#define NVS_NAMESPACE           "shelly_meter"
#define NVS_KEY_ENERGY          "energy_mwh"
#define NVS_KEY_CF_BASE         "cf_base"
#define NVS_KEY_AC_SEEN         "ac_seen"

// ---------------------------------------------------------------------------
// Electrical Power Measurement cluster
// ---------------------------------------------------------------------------
//
// esp-matter's own EPM Instance hides its cluster, so a value change can
// only be reported with MatterReportingAttributeChangeCallback(). That
// bumps esp-matter's copy of the cluster data version, but a registered
// code-driven cluster reports its own GetDataVersion(): a controller that
// resubscribes with a DataVersionFilter would skip the changed values. This
// subclass exposes NotifyAttributeChanged(), which bumps the version the
// cluster actually reports, and is registered here instead of through
// esp-matter's delegate init callback.

namespace {

class ShellyEpmCluster : public EPM::ElectricalPowerMeasurementCluster
{
public:
    using EPM::ElectricalPowerMeasurementCluster::ElectricalPowerMeasurementCluster;
    void MarkChanged(AttributeId attribute_id) { NotifyAttributeChanged(attribute_id); }
};

using EPM::Delegate;
using EPM::MeasurementTypeEnum;
using EPM::PowerModeEnum;
namespace Structs = EPM::Structs;

const Structs::MeasurementAccuracyRangeStruct::Type kPowerRange[] = {
    {.rangeMin = 0, .rangeMax = 8'000'000, .percentMax = MakeOptional(static_cast<Percent100ths>(200))}};
const Structs::MeasurementAccuracyRangeStruct::Type kVoltageRange[] = {
    {.rangeMin = 0, .rangeMax = 300'000, .percentMax = MakeOptional(static_cast<Percent100ths>(200))}};
const Structs::MeasurementAccuracyRangeStruct::Type kCurrentRange[] = {
    {.rangeMin = 0, .rangeMax = 30'000, .percentMax = MakeOptional(static_cast<Percent100ths>(200))}};
const Structs::MeasurementAccuracyRangeStruct::Type kFrequencyRange[] = {
    {.rangeMin = 45'000, .rangeMax = 65'000, .percentMax = MakeOptional(static_cast<Percent100ths>(200))}};

// ±2%, from the per-board calibration. Units: mW, mV, mA, mHz.
const Structs::MeasurementAccuracyStruct::Type kAccuracy[] = {
    {.measurementType = MeasurementTypeEnum::kActivePower, .measured = true, .minMeasuredValue = 0,
     .maxMeasuredValue = 8'000'000, .accuracyRanges = DataModel::List<const Structs::MeasurementAccuracyRangeStruct::Type>(kPowerRange)},
    {.measurementType = MeasurementTypeEnum::kVoltage, .measured = true, .minMeasuredValue = 0,
     .maxMeasuredValue = 300'000, .accuracyRanges = DataModel::List<const Structs::MeasurementAccuracyRangeStruct::Type>(kVoltageRange)},
    {.measurementType = MeasurementTypeEnum::kActiveCurrent, .measured = true, .minMeasuredValue = 0,
     .maxMeasuredValue = 30'000, .accuracyRanges = DataModel::List<const Structs::MeasurementAccuracyRangeStruct::Type>(kCurrentRange)},
    {.measurementType = MeasurementTypeEnum::kFrequency, .measured = true, .minMeasuredValue = 45'000,
     .maxMeasuredValue = 65'000, .accuracyRanges = DataModel::List<const Structs::MeasurementAccuracyRangeStruct::Type>(kFrequencyRange)},
};

// Values are only written on the Matter thread (publish_work), where the
// cluster also reads them, so they need no lock.
class ShellyEpmDelegate : public Delegate
{
public:
    PowerModeEnum GetPowerMode() override { return PowerModeEnum::kAc; }
    uint8_t GetNumberOfMeasurementTypes() override { return MATTER_ARRAY_SIZE(kAccuracy); }

    CHIP_ERROR StartAccuracyRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetAccuracyByIndex(uint8_t index, Structs::MeasurementAccuracyStruct::Type &accuracy) override
    {
        VerifyOrReturnError(index < MATTER_ARRAY_SIZE(kAccuracy), CHIP_ERROR_PROVIDER_LIST_EXHAUSTED);
        accuracy = kAccuracy[index];
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR EndAccuracyRead() override { return CHIP_NO_ERROR; }

    CHIP_ERROR StartRangesRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetRangeByIndex(uint8_t, Structs::MeasurementRangeStruct::Type &) override
    {
        return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
    }
    CHIP_ERROR EndRangesRead() override { return CHIP_NO_ERROR; }

    CHIP_ERROR StartHarmonicCurrentsRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetHarmonicCurrentsByIndex(uint8_t, Structs::HarmonicMeasurementStruct::Type &) override
    {
        return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
    }
    CHIP_ERROR EndHarmonicCurrentsRead() override { return CHIP_NO_ERROR; }

    CHIP_ERROR StartHarmonicPhasesRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetHarmonicPhasesByIndex(uint8_t, Structs::HarmonicMeasurementStruct::Type &) override
    {
        return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
    }
    CHIP_ERROR EndHarmonicPhasesRead() override { return CHIP_NO_ERROR; }

    DataModel::Nullable<int64_t> GetVoltage() override { return mVoltage; }
    DataModel::Nullable<int64_t> GetActiveCurrent() override { return mActiveCurrent; }
    DataModel::Nullable<int64_t> GetReactiveCurrent() override { return {}; }
    DataModel::Nullable<int64_t> GetApparentCurrent() override { return {}; }
    // Never null: Home Assistant drops a power entity whose value is null
    // when it discovers the device.
    DataModel::Nullable<int64_t> GetActivePower() override { return mActivePower; }
    DataModel::Nullable<int64_t> GetReactivePower() override { return {}; }
    DataModel::Nullable<int64_t> GetApparentPower() override { return {}; }
    DataModel::Nullable<int64_t> GetRMSVoltage() override { return {}; }
    DataModel::Nullable<int64_t> GetRMSCurrent() override { return {}; }
    DataModel::Nullable<int64_t> GetRMSPower() override { return {}; }
    DataModel::Nullable<int64_t> GetFrequency() override { return mFrequency; }
    DataModel::Nullable<int64_t> GetPowerFactor() override { return {}; }
    DataModel::Nullable<int64_t> GetNeutralCurrent() override { return {}; }

    DataModel::Nullable<int64_t> mVoltage;
    DataModel::Nullable<int64_t> mActiveCurrent;
    DataModel::Nullable<int64_t> mActivePower = DataModel::MakeNullable<int64_t>(0);
    DataModel::Nullable<int64_t> mFrequency;
};

ShellyEpmDelegate s_epm_delegate;
RegisteredServerCluster<ShellyEpmCluster> *s_epm_cluster = nullptr;

} // namespace

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static bool s_ac_seen = false;          // NVS latch
static bool s_advertised = false;       // Decided once, before the node is created
static uint16_t s_endpoint_id = 0;
static volatile bool s_matter_ready = false;

static energy_acc_t s_energy;
static energy_save_policy_t s_save;

// Latest sample, written by the meter task, read by publish_work.
typedef struct {
    bool valid;             // A valid frame arrived within METER_STALE_MS
    float volts;
    float amps;
    float watts;
    float hz;
    bool commanded_on;
    int64_t total_mwh;
} meter_snapshot_t;

static portMUX_TYPE s_snap_mux = portMUX_INITIALIZER_UNLOCKED;
static meter_snapshot_t s_snap = {};

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

static void nvs_load(int64_t *total_mwh, uint32_t *cf_base, bool *have_energy)
{
    nvs_handle_t h;
    *have_energy = false;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t ac = 0;
    if (nvs_get_u8(h, NVS_KEY_AC_SEEN, &ac) == ESP_OK) {
        s_ac_seen = ac != 0;
    }
    // Outputs change only when both keys read and the total is sane.
    int64_t total = 0;
    uint32_t base = 0;
    if (nvs_get_i64(h, NVS_KEY_ENERGY, &total) == ESP_OK && nvs_get_u32(h, NVS_KEY_CF_BASE, &base) == ESP_OK &&
        total >= 0) {
        *total_mwh = total;
        *cf_base = base;
        *have_energy = true;
    }
    nvs_close(h);
}

static void nvs_save_energy(int64_t total_mwh, uint32_t cf_base)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_i64(h, NVS_KEY_ENERGY, total_mwh);
        if (err == ESP_OK) {
            err = nvs_set_u32(h, NVS_KEY_CF_BASE, cf_base);
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save energy: %s", esp_err_to_name(err));
    }
}

static void nvs_save_ac_seen(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, NVS_KEY_AC_SEEN, 1);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save the AC latch: %s", esp_err_to_name(err));
    }
}

// ---------------------------------------------------------------------------
// Matter reporting (Matter thread)
// ---------------------------------------------------------------------------

static DataModel::Nullable<int64_t> milli(float value)
{
    if (isnan(value)) {
        return DataModel::Nullable<int64_t>();
    }
    return DataModel::MakeNullable<int64_t>((int64_t)lroundf(value * 1000.0f));
}

static float unmilli(const DataModel::Nullable<int64_t> &value)
{
    return value.IsNull() ? NAN : (float)value.Value() / 1000.0f;
}

static void publish_work(intptr_t arg)
{
    static int64_t last_heartbeat_ms = 0;
    meter_snapshot_t snap;
    portENTER_CRITICAL(&s_snap_mux);
    snap = s_snap;
    portEXIT_CRITICAL(&s_snap_mux);

    if (s_epm_cluster == nullptr) {
        return;
    }
    int64_t now_ms = esp_timer_get_time() / 1000;
    bool heartbeat = now_ms - last_heartbeat_ms >= HEARTBEAT_MS;
    if (heartbeat) {
        last_heartbeat_ms = now_ms;
    }

    float volts = snap.valid ? snap.volts : NAN;
    float amps = snap.valid ? snap.amps : NAN;
    float hz = snap.valid ? snap.hz : NAN;
    // ActivePower is never null. With the meter lost it reads 0 while the
    // relay is commanded off, and otherwise holds its last value.
    float watts = snap.valid ? snap.watts : (snap.commanded_on ? unmilli(s_epm_delegate.mActivePower) : 0.0f);

    ShellyEpmCluster &cluster = s_epm_cluster->Cluster();
    if (heartbeat || report_changed(unmilli(s_epm_delegate.mActivePower), watts, POWER_STEP_W, POWER_STEP_REL)) {
        s_epm_delegate.mActivePower = milli(watts);
        cluster.MarkChanged(EPM::Attributes::ActivePower::Id);
    }
    if (heartbeat || report_changed(unmilli(s_epm_delegate.mVoltage), volts, VOLTAGE_STEP_V, 0.0f)) {
        s_epm_delegate.mVoltage = milli(volts);
        cluster.MarkChanged(EPM::Attributes::Voltage::Id);
    }
    if (heartbeat || report_changed(unmilli(s_epm_delegate.mActiveCurrent), amps, CURRENT_STEP_A, 0.0f)) {
        s_epm_delegate.mActiveCurrent = milli(amps);
        cluster.MarkChanged(EPM::Attributes::ActiveCurrent::Id);
    }
    if (heartbeat || report_changed(unmilli(s_epm_delegate.mFrequency), hz, FREQ_STEP_HZ, 0.0f)) {
        s_epm_delegate.mFrequency = milli(hz);
        cluster.MarkChanged(EPM::Attributes::Frequency::Id);
    }
}

// Each call emits a CumulativeEnergyMeasured event, so this runs on an
// energy change of 10 Wh or every 5 minutes, never at the poll rate.
static void energy_work(intptr_t arg)
{
    int64_t total_mwh;
    portENTER_CRITICAL(&s_snap_mux);
    total_mwh = s_snap.total_mwh;
    portEXIT_CRITICAL(&s_snap_mux);

    ElectricalEnergyMeasurement::Structs::EnergyMeasurementStruct::Type energy;
    energy.energy = total_mwh;
    energy.endSystime.SetValue(static_cast<uint64_t>(System::SystemClock().GetMonotonicTimestamp().count()));
    if (!ElectricalEnergyMeasurement::NotifyCumulativeEnergyMeasured(s_endpoint_id, MakeOptional(energy), NullOptional)) {
        ESP_LOGW(TAG, "Energy cluster not ready");
    }
}

static void schedule(void (*work)(intptr_t))
{
    if (!s_matter_ready) {
        return;
    }
    CHIP_ERROR err = DeviceLayer::PlatformMgr().ScheduleWork(work, 0);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGW(TAG, "Failed to schedule a report: %" CHIP_ERROR_FORMAT, err.Format());
    }
}

// ---------------------------------------------------------------------------
// Meter task
// ---------------------------------------------------------------------------

static void meter_task(void *arg)
{
    const bl0942_cal_t *cal = bl0942_calibration();
    ac_detect_t ac;
    ac_detect_init(&ac, AC_DETECT_V, AC_DETECT_COUNT);

    int64_t last_valid_ms = esp_timer_get_time() / 1000;
    int64_t last_publish_ms = 0;
    int64_t last_notify_ms = 0;
    int64_t last_notified_mwh = energy_acc_total_mwh(&s_energy);
    bl0942_reading_t reading = {};
    reading.hz = NAN;
    bool fresh = false;     // A valid frame arrived since the last publish

    while (true) {
        int64_t now_us = esp_timer_get_time();
        int64_t now_ms = now_us / 1000;

        bl0942_raw_t raw;
        protection_sample_t sample = {};
        sample.now_us = now_us;
        if (bl0942_read(&raw) == ESP_OK) {
            bl0942_scale(cal, &raw, &reading);
            sample.valid = true;
            sample.volts = reading.volts;
            sample.amps = reading.amps;
            sample.amps_clipped = reading.amps_clipped;
            last_valid_ms = now_ms;
            fresh = true;

            energy_result_t r = energy_acc_update(&s_energy, raw.cf_cnt, now_ms);
            if (r == ENERGY_CHIP_RESET) {
                ESP_LOGW(TAG, "Meter counter went back (chip reset): delta dropped");
            } else if (r == ENERGY_GLITCH) {
                ESP_LOGW(TAG, "Implausible energy delta dropped");
            }
        }
        // Protection first, every poll.
        protection_feed(&sample);

        int64_t total_mwh = energy_acc_total_mwh(&s_energy);
        if (energy_save_due(&s_save, total_mwh, now_ms)) {
            nvs_save_energy(total_mwh, s_energy.last_cf);
            energy_save_done(&s_save, total_mwh, now_ms);
        }

        if (now_ms - last_publish_ms >= PUBLISH_MS) {
            last_publish_ms = now_ms;
            bool valid = now_ms - last_valid_ms < METER_STALE_MS;

            // Seeing AC for the first time latches metering on from the
            // next boot; the composition never changes while running.
            if (!s_ac_seen && fresh && ac_detect_update(&ac, reading.volts)) {
                s_ac_seen = true;
                nvs_save_ac_seen();
                ESP_LOGW(TAG, "AC mains detected: metering is advertised from the next boot");
            }

            fresh = false;

            portENTER_CRITICAL(&s_snap_mux);
            s_snap.valid = valid;
            s_snap.volts = reading.volts;
            s_snap.amps = reading.amps;
            s_snap.watts = reading.watts;
            s_snap.hz = reading.hz;
            s_snap.commanded_on = relay_get();
            s_snap.total_mwh = total_mwh;
            portEXIT_CRITICAL(&s_snap_mux);

            if (s_advertised) {
                schedule(publish_work);
                if (total_mwh - last_notified_mwh >= NOTIFY_DELTA_MWH || now_ms - last_notify_ms >= NOTIFY_INTERVAL_MS) {
                    last_notified_mwh = total_mwh;
                    last_notify_ms = now_ms;
                    schedule(energy_work);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

// Poll for up to 3s: is this board on AC mains? Also measures the line
// frequency and reconfigures the meter if it differs from the default.
static bool detect_ac(bool *line_60hz)
{
    const bl0942_cal_t *cal = bl0942_calibration();
    ac_detect_t ac;
    ac_detect_init(&ac, AC_DETECT_V, AC_DETECT_COUNT);
    float hz = NAN;

    for (int elapsed = 0; elapsed < AC_DETECT_WINDOW_MS; elapsed += AC_DETECT_SAMPLE_MS) {
        bl0942_raw_t raw;
        if (bl0942_read(&raw) == ESP_OK) {
            bl0942_reading_t r;
            bl0942_scale(cal, &raw, &r);
            hz = r.hz;
            if (ac_detect_update(&ac, r.volts)) {
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(AC_DETECT_SAMPLE_MS));
    }

    if (ac.present && !isnan(hz)) {
        if (hz >= 45.0f && hz < 55.0f && *line_60hz) {
            *line_60hz = false;
            bl0942_set_line_frequency(false);
        } else if (hz >= 55.0f && hz <= 65.0f && !*line_60hz) {
            *line_60hz = true;
            bl0942_set_line_frequency(true);
        }
    }
    return ac.present;
}

esp_err_t metering_init(void)
{
    bool line_60hz = true;
#ifndef CONFIG_SHELLY_BL0942_LINE_60HZ
    line_60hz = false;
#endif
    esp_err_t err = bl0942_init(line_60hz);
    if (err != ESP_OK) {
        // Keep going: frames may still arrive, and if not the meter-lost
        // alert is raised. The relay keeps working either way.
        ESP_LOGE(TAG, "Meter setup failed: %s", esp_err_to_name(err));
    }

    int64_t total_mwh = 0;
    uint32_t cf_base = 0;
    bool have_energy = false;
    nvs_load(&total_mwh, &cf_base, &have_energy);

    bool ac_now = detect_ac(&line_60hz);
    if (ac_now && !s_ac_seen) {
        s_ac_seen = true;
        nvs_save_ac_seen();
    }
    s_advertised = s_ac_seen;
    ESP_LOGI(TAG, "AC %s now, %s: metering %s", ac_now ? "present" : "not detected",
             s_ac_seen ? "seen before" : "never seen", s_advertised ? "advertised" : "not advertised");

    const bl0942_cal_t *cal = bl0942_calibration();
    int64_t now_ms = esp_timer_get_time() / 1000;
    energy_acc_init(&s_energy, bl0942_mwh_per_pulse(cal), MAX_PLAUSIBLE_W, (double)RESTORE_SLACK_MWH);
    if (have_energy) {
        // After a software reset the meter kept counting: resume from the
        // saved baseline. After a power loss the counter restarted and the
        // first sample reads as a chip reset.
        energy_acc_restore(&s_energy, total_mwh, cf_base, now_ms);
    }
    energy_save_policy_init(&s_save, SAVE_DELTA_MWH, SAVE_INTERVAL_MS, total_mwh, now_ms);
    ESP_LOGI(TAG, "Energy total %lld Wh", (long long)(total_mwh / 1000));

    s_snap.total_mwh = total_mwh;
    s_snap.hz = NAN;

    if (xTaskCreate(meter_task, "meter", TASK_STACK_SIZE, nullptr, TASK_PRIORITY, nullptr) != pdPASS) {
        // Without the task there is no protection. The breaker still is.
        ESP_LOGE(TAG, "Failed to create the meter task: electrical protection is NOT running");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t metering_add_to_endpoint(endpoint_t *endpoint)
{
    if (!s_advertised) {
        return ESP_OK;
    }
    uint16_t endpoint_id = endpoint::get_id(endpoint);

    endpoint::electrical_sensor::config_t sensor_config;
    sensor_config.electrical_power_measurement.feature_flags =
        cluster::electrical_power_measurement::feature::alternating_current::get_id();
    // No esp-matter delegate: the cluster is registered below (see
    // ShellyEpmCluster). PowerTopology is left alone: legacy
    // electrical_sensor::add() forces NodeTopology, and any other topology
    // flag aborts at boot (exactly one topology allowed).
    sensor_config.electrical_power_measurement.delegate = nullptr;

    // esp-matter asserts (aborts, on every boot) on an invalid feature set.
    // Refuse here instead, so a bad change costs metering, not the device.
    if (sensor_config.power_topology.feature_flags != 0 ||
        sensor_config.electrical_power_measurement.feature_flags == 0) {
        ESP_LOGE(TAG, "Invalid metering composition, metering disabled");
        s_advertised = false;
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = endpoint::electrical_sensor::add(endpoint, &sensor_config);
    cluster_t *epm = cluster::get(endpoint, EPM::Id);
    if (err != ESP_OK || epm == nullptr) {
        ESP_LOGE(TAG, "Failed to add the Electrical Sensor device type");
        s_advertised = false;
        return err != ESP_OK ? err : ESP_FAIL;
    }
    // create() only adds ActivePower, Accuracy, PowerMode and
    // NumberOfMeasurementTypes.
    cluster::electrical_power_measurement::attribute::create_voltage(epm, nullable<int64_t>());
    cluster::electrical_power_measurement::attribute::create_active_current(epm, nullable<int64_t>());
    cluster::electrical_power_measurement::attribute::create_frequency(epm, nullable<int64_t>());

    cluster::electrical_energy_measurement::config_t energy_config;
    energy_config.feature_flags = cluster::electrical_energy_measurement::feature::imported_energy::get_id() |
                                  cluster::electrical_energy_measurement::feature::cumulative_energy::get_id();
    if (cluster::electrical_energy_measurement::create(endpoint, &energy_config, CLUSTER_FLAG_SERVER) == nullptr) {
        ESP_LOGE(TAG, "Failed to create the Electrical Energy Measurement cluster");
    }

    EPM::ElectricalPowerMeasurementCluster::OptionalAttributesSet optional;
    optional.Set<EPM::Attributes::Voltage::Id>();
    optional.Set<EPM::Attributes::ActiveCurrent::Id>();
    optional.Set<EPM::Attributes::Frequency::Id>();
    s_epm_cluster = new RegisteredServerCluster<ShellyEpmCluster>(EPM::ElectricalPowerMeasurementCluster::Config{
        .endpointId = endpoint_id,
        .delegate = s_epm_delegate,
        .features = BitMask<EPM::Feature>(EPM::Feature::kAlternatingCurrent),
        .optionalAttributes = optional,
    });
    CHIP_ERROR chip_err = esp_matter::data_model::provider::get_instance().registry().Register(
        s_epm_cluster->Registration());
    if (chip_err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Failed to register the power measurement cluster: %" CHIP_ERROR_FORMAT, chip_err.Format());
    }

    s_endpoint_id = endpoint_id;
    ESP_LOGI(TAG, "Metering on endpoint %u", endpoint_id);
    return ESP_OK;
}

// The cluster keeps the accuracy struct by value, and its accuracyRanges
// list only points at this array, so the array must outlive the cluster.
static const ElectricalEnergyMeasurement::Structs::MeasurementAccuracyRangeStruct::Type kEnergyRanges[] = {
    {.rangeMin = 0, .rangeMax = 1'000'000'000'000'000, .percentMax = MakeOptional(static_cast<Percent100ths>(200))}};

static void matter_started_work(intptr_t arg)
{
    ElectricalEnergyMeasurement::Structs::MeasurementAccuracyStruct::Type accuracy = {
        .measurementType = ElectricalEnergyMeasurement::MeasurementTypeEnum::kElectricalEnergy,
        .measured = true,
        .minMeasuredValue = 0,
        .maxMeasuredValue = 1'000'000'000'000'000,
        .accuracyRanges =
            DataModel::List<const ElectricalEnergyMeasurement::Structs::MeasurementAccuracyRangeStruct::Type>(kEnergyRanges),
    };
    CHIP_ERROR err = ElectricalEnergyMeasurement::SetMeasurementAccuracy(s_endpoint_id, accuracy);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGW(TAG, "Failed to set the energy accuracy: %" CHIP_ERROR_FORMAT, err.Format());
    }
    // Publish the restored total now, so CumulativeEnergyImported is not
    // null until the first 5-minute report.
    energy_work(0);
    publish_work(0);
}

void metering_matter_started(void)
{
    if (!s_advertised || s_endpoint_id == 0) {
        return;
    }
    s_matter_ready = true;
    schedule(matter_started_work);
}

#endif // CONFIG_SHELLY_HAS_BL0942
