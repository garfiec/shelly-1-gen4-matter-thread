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

#ifdef CONFIG_SHELLY_HAS_METER

#include "metering.h"
#include "metering_live.h"
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

#if CONFIG_SHELLY_HAS_BL0942
#include <bl0942.h>
#elif CONFIG_SHELLY_HAS_ADE7953
#include <ade7953.h>
#else
#error "CONFIG_SHELLY_HAS_METER needs a meter: CONFIG_SHELLY_HAS_BL0942 or CONFIG_SHELLY_HAS_ADE7953"
#endif

#include <protection.h>
#include <protection_logic.h>
#include <relay.h>
#include <shelly_calibration.h>

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters;
using namespace esp_matter;

namespace EPM = chip::app::Clusters::ElectricalPowerMeasurement;

static const char *TAG = "metering";

#define CHANNELS RELAY_CHANNELS
static_assert(CHANNELS <= METERING_LIVE_CHANNELS && CHANNELS <= PROTECTION_MAX_CHANNELS &&
                  CHANNELS <= CAL_CHANNELS,
              "metering holds too few channels");

#define POLL_MS                 200
#define PUBLISH_MS              1000
#define HEARTBEAT_MS            60000
#define METER_STALE_MS          (CONFIG_SHELLY_PROT_METER_LOST_S * 1000LL)
#define TASK_STACK_SIZE         5120
#define TASK_PRIORITY           8

// AC detection at boot: V_RMS >= 80 V on 3 consecutive samples, 400ms
// apart, within 3s. Three samples span more than one 800ms RMS update
// (the BL0942's MODE), so at least two different updates agree.
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
// Energy from power (a meter without a pulse counter): a stalled poll
// counts at most this long.
#define MAX_INTEGRATE_MS        5000

// Report thresholds.
#define POWER_STEP_W            1.0f
#define POWER_STEP_REL          0.02f
#define VOLTAGE_STEP_V          1.0f
#define CURRENT_STEP_A          0.05f
#define FREQ_STEP_HZ            0.1f

// The 2PM's ADE7953 reads about 0.07 A of noise with nothing connected.
// Reported readings under 1 W and 0.15 A are 0 (noload_cut()).
#define NOLOAD_W                1.0f
#define NOLOAD_A                0.15f

// The calibration average: 25 polls, 5 s.
#define CAL_WINDOW_POLLS        25

// Stored energy, the AC latch and the calibration survive a factory reset:
// they describe the hardware and its history, not the Matter commissioning.
#define NVS_NAMESPACE           "shelly_meter"
#define NVS_KEY_CF_BASE         "cf_base"
#define NVS_KEY_AC_SEEN         "ac_seen"
// Relay 1 keeps the single-relay key.
static const char *const NVS_KEY_ENERGY[] = {"energy_mwh", "energy_mwh2"};

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

ShellyEpmDelegate s_epm_delegate[CHANNELS];
RegisteredServerCluster<ShellyEpmCluster> *s_epm_cluster[CHANNELS] = {};

} // namespace

// ---------------------------------------------------------------------------
// The meter chip
// ---------------------------------------------------------------------------
//
// One reading of the chip, scaled with the compiled calibration only.

typedef struct {
    float volts;
    float hz;
    float amps[CHANNELS];
    float watts[CHANNELS];
    bool amps_clipped[CHANNELS];
    uint32_t cf_cnt;                // BL0942 only: the energy pulse counter
    int64_t raw_v;
    int64_t raw_i[CHANNELS];
    int64_t raw_p[CHANNELS];
} chip_reading_t;

#if CONFIG_SHELLY_HAS_BL0942

#define CHIP_NAME               "BL0942"
#define HAS_PULSE_COUNTER       1
#define CURRENT_FULL_SCALE_A    (CONFIG_SHELLY_BL0942_CURRENT_FULL_SCALE_MA / 1000.0f)

static bool chip_read(chip_reading_t *r)
{
    bl0942_raw_t raw;
    if (bl0942_read(&raw) != ESP_OK) {
        return false;
    }
    bl0942_reading_t reading;
    bl0942_scale(bl0942_calibration(), &raw, &reading);
    r->volts = reading.volts;
    r->hz = reading.hz;
    r->amps[0] = reading.amps;
    r->watts[0] = reading.watts;
    r->amps_clipped[0] = reading.amps_clipped;
    r->cf_cnt = raw.cf_cnt;
    r->raw_v = raw.v_rms;
    r->raw_i[0] = raw.i_rms;
    r->raw_p[0] = raw.watt;
    return true;
}

// Units per count, for the calibration (1.0 for the relay it doesn't have).
static void chip_base(float base[CAL_Q_COUNT])
{
    const bl0942_cal_t *cal = bl0942_calibration();
    for (int i = 0; i < CAL_Q_COUNT; i++) {
        base[i] = 1.0f;
    }
    base[CAL_Q_V] = 1.0f / cal->voltage_ref;
    base[CAL_Q_I0] = 1.0f / cal->current_ref;
    base[CAL_Q_P0] = 1.0f / cal->power_ref;
}

#else // CONFIG_SHELLY_HAS_ADE7953

#define CHIP_NAME               "ADE7953"
#define HAS_PULSE_COUNTER       0

static bool chip_read(chip_reading_t *r)
{
    ade7953_raw_t raw;
    if (ade7953_read(&raw) != ESP_OK) {
        return false;
    }
    ade7953_reading_t reading;
    ade7953_scale(ade7953_calibration(), &raw, &reading);
    r->volts = reading.volts;
    r->hz = reading.hz;
    r->raw_v = raw.vrms;
    for (int ch = 0; ch < CHANNELS; ch++) {
        r->amps[ch] = reading.amps[ch];
        r->watts[ch] = reading.watts[ch];
        r->amps_clipped[ch] = false;
        r->raw_i[ch] = raw.irms[ch];
        r->raw_p[ch] = raw.watt[ch];
    }
    r->cf_cnt = 0;
    return true;
}

static void chip_base(float base[CAL_Q_COUNT])
{
    const ade7953_cal_t *cal = ade7953_calibration();
    for (int i = 0; i < CAL_Q_COUNT; i++) {
        base[i] = 1.0f;
    }
    base[CAL_Q_V] = ade7953_volts_per_count(cal);
    for (int ch = 0; ch < CHANNELS; ch++) {
        base[CAL_Q_I(ch)] = ade7953_amps_per_count(cal, ch);
        base[CAL_Q_P(ch)] = ade7953_watts_per_count(cal, ch);
    }
}

#endif

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static bool s_ac_seen = false;          // NVS latch
static bool s_advertised = false;       // Decided once, before the node is created
static uint16_t s_endpoint_id[CHANNELS];
static volatile bool s_matter_ready = false;

#if HAS_PULSE_COUNTER
static energy_acc_t s_energy;
static double s_mwh_per_pulse = 0.0;    // From the compiled calibration
#else
static double s_energy_mwh[CHANNELS];
#endif
static energy_save_policy_t s_save[CHANNELS];

// Latest sample, written by the meter task, read by publish_work and the
// maintenance page.
typedef struct {
    bool valid;             // A valid reading arrived within METER_STALE_MS
    float volts;
    float hz;
    float amps[CHANNELS];
    float watts[CHANNELS];
    bool commanded_on[CHANNELS];
    int64_t total_mwh[CHANNELS];
    int64_t raw_v;
    int64_t raw_i[CHANNELS];
    int64_t raw_p[CHANNELS];
} meter_snapshot_t;

static portMUX_TYPE s_snap_mux = portMUX_INITIALIZER_UNLOCKED;
static meter_snapshot_t s_snap = {};

// The calibration average (under s_snap_mux): voltage, and each relay's
// current and power, one entry per poll.
static avg_window_t s_avg_v;
static avg_window_t s_avg_a[CHANNELS];
static avg_window_t s_avg_w[CHANNELS];

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

static void nvs_load(int64_t total_mwh[CHANNELS], uint32_t *cf_base, bool *have_energy)
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
#if HAS_PULSE_COUNTER
    // Outputs change only when both keys read and the total is sane.
    int64_t total = 0;
    uint32_t base = 0;
    if (nvs_get_i64(h, NVS_KEY_ENERGY[0], &total) == ESP_OK && nvs_get_u32(h, NVS_KEY_CF_BASE, &base) == ESP_OK &&
        total >= 0) {
        total_mwh[0] = total;
        *cf_base = base;
        *have_energy = true;
    }
#else
    for (int ch = 0; ch < CHANNELS; ch++) {
        int64_t total = 0;
        if (nvs_get_i64(h, NVS_KEY_ENERGY[ch], &total) == ESP_OK && total >= 0) {
            total_mwh[ch] = total;
            *have_energy = true;
        }
    }
#endif
    nvs_close(h);
}

static void nvs_save_energy(int ch, int64_t total_mwh, uint32_t cf_base)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_i64(h, NVS_KEY_ENERGY[ch], total_mwh);
#if HAS_PULSE_COUNTER
        if (err == ESP_OK) {
            err = nvs_set_u32(h, NVS_KEY_CF_BASE, cf_base);
        }
#endif
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

static int64_t energy_total_mwh(int ch)
{
#if HAS_PULSE_COUNTER
    return energy_acc_total_mwh(&s_energy);
#else
    return (int64_t)s_energy_mwh[ch];
#endif
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

    int64_t now_ms = esp_timer_get_time() / 1000;
    bool heartbeat = now_ms - last_heartbeat_ms >= HEARTBEAT_MS;
    if (heartbeat) {
        last_heartbeat_ms = now_ms;
    }

    for (int ch = 0; ch < CHANNELS; ch++) {
        if (s_epm_cluster[ch] == nullptr) {
            continue;
        }
        ShellyEpmDelegate &d = s_epm_delegate[ch];
        float volts = snap.valid ? snap.volts : NAN;
        float amps = snap.valid ? snap.amps[ch] : NAN;
        float hz = snap.valid ? snap.hz : NAN;
        // ActivePower is never null. With the meter lost it reads 0 while the
        // relay is commanded off, and otherwise holds its last value.
        float watts = snap.valid ? snap.watts[ch] : (snap.commanded_on[ch] ? unmilli(d.mActivePower) : 0.0f);

        ShellyEpmCluster &cluster = s_epm_cluster[ch]->Cluster();
        if (heartbeat || report_changed(unmilli(d.mActivePower), watts, POWER_STEP_W, POWER_STEP_REL)) {
            d.mActivePower = milli(watts);
            cluster.MarkChanged(EPM::Attributes::ActivePower::Id);
        }
        if (heartbeat || report_changed(unmilli(d.mVoltage), volts, VOLTAGE_STEP_V, 0.0f)) {
            d.mVoltage = milli(volts);
            cluster.MarkChanged(EPM::Attributes::Voltage::Id);
        }
        if (heartbeat || report_changed(unmilli(d.mActiveCurrent), amps, CURRENT_STEP_A, 0.0f)) {
            d.mActiveCurrent = milli(amps);
            cluster.MarkChanged(EPM::Attributes::ActiveCurrent::Id);
        }
        if (heartbeat || report_changed(unmilli(d.mFrequency), hz, FREQ_STEP_HZ, 0.0f)) {
            d.mFrequency = milli(hz);
            cluster.MarkChanged(EPM::Attributes::Frequency::Id);
        }
    }
}

// Each call emits a CumulativeEnergyMeasured event per relay, so this runs
// on an energy change of 10 Wh or every 5 minutes, never at the poll rate.
static void energy_work(intptr_t arg)
{
    int64_t total_mwh[CHANNELS];
    portENTER_CRITICAL(&s_snap_mux);
    for (int ch = 0; ch < CHANNELS; ch++) {
        total_mwh[ch] = s_snap.total_mwh[ch];
    }
    portEXIT_CRITICAL(&s_snap_mux);

    for (int ch = 0; ch < CHANNELS; ch++) {
        if (s_endpoint_id[ch] == 0) {
            continue;
        }
        ElectricalEnergyMeasurement::Structs::EnergyMeasurementStruct::Type energy;
        energy.energy = total_mwh[ch];
        energy.endSystime.SetValue(static_cast<uint64_t>(System::SystemClock().GetMonotonicTimestamp().count()));
        if (!ElectricalEnergyMeasurement::NotifyCumulativeEnergyMeasured(s_endpoint_id[ch], MakeOptional(energy),
                                                                         NullOptional)) {
            ESP_LOGW(TAG, "Energy cluster not ready");
        }
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

// Apply a changed calibration where it is not applied per reading.
static void calibration_changed(const cal_ratios_t &cal)
{
#if HAS_PULSE_COUNTER
    // Energy counted from here on uses the corrected power scale.
    s_energy.mwh_per_pulse = s_mwh_per_pulse * cal.q[CAL_Q_P0];
#endif
    // The average spans readings with one calibration.
    portENTER_CRITICAL(&s_snap_mux);
    avg_window_push(&s_avg_v, NAN);
    for (int ch = 0; ch < CHANNELS; ch++) {
        avg_window_push(&s_avg_a[ch], NAN);
        avg_window_push(&s_avg_w[ch], NAN);
    }
    portEXIT_CRITICAL(&s_snap_mux);
}

static void meter_task(void *arg)
{
    ac_detect_t ac;
    ac_detect_init(&ac, AC_DETECT_V, AC_DETECT_COUNT);

    int64_t last_valid_ms = esp_timer_get_time() / 1000;
    int64_t last_publish_ms = 0;
    int64_t last_notify_ms = 0;
    int64_t last_notified_mwh[CHANNELS];
    for (int ch = 0; ch < CHANNELS; ch++) {
        last_notified_mwh[ch] = energy_total_mwh(ch);
    }
    chip_reading_t reading = {};
    reading.hz = NAN;
    bool fresh = false;     // A valid reading arrived since the last publish
    uint32_t cal_generation = 0;
    cal_ratios_t cal;
    cal_ratios_default(&cal);

    while (true) {
        int64_t now_us = esp_timer_get_time();
        int64_t now_ms = now_us / 1000;

        uint32_t generation = shelly_cal_generation();
        if (generation != cal_generation) {
            cal_generation = generation;
            shelly_cal_get(&cal);
            calibration_changed(cal);
        }

        protection_sample_t sample = {};
        sample.now_us = now_us;
        bool got = chip_read(&reading);
        if (got) {
            reading.volts *= cal.q[CAL_Q_V];
            for (int ch = 0; ch < CHANNELS; ch++) {
                reading.amps[ch] *= cal.q[CAL_Q_I(ch)];
                reading.watts[ch] *= cal.q[CAL_Q_P(ch)];
            }
            sample.valid = true;
            sample.volts = reading.volts;
            for (int ch = 0; ch < CHANNELS; ch++) {
                float amps = reading.amps[ch];
#if HAS_PULSE_COUNTER
                // A clipped current channel reads low; count it as at least full scale.
                if (reading.amps_clipped[ch] && amps < CURRENT_FULL_SCALE_A) {
                    amps = CURRENT_FULL_SCALE_A;
                }
#endif
                sample.amps[ch] = amps;
            }

#if HAS_PULSE_COUNTER
            energy_result_t r = energy_acc_update(&s_energy, reading.cf_cnt, now_ms);
            if (r == ENERGY_CHIP_RESET) {
                ESP_LOGW(TAG, "Meter counter went back (chip reset): delta dropped");
            } else if (r == ENERGY_GLITCH) {
                ESP_LOGW(TAG, "Implausible energy delta dropped");
            }
#else
            // Energy from the reported power: the noise of an idle channel
            // counts nothing. dt runs from the previous valid reading.
            for (int ch = 0; ch < CHANNELS; ch++) {
                noload_cut(&reading.amps[ch], &reading.watts[ch], NOLOAD_W, NOLOAD_A);
                s_energy_mwh[ch] += power_to_mwh(reading.watts[ch], now_ms - last_valid_ms, MAX_INTEGRATE_MS);
            }
#endif
            last_valid_ms = now_ms;
            fresh = true;
        }
        // Protection first, every poll.
        protection_feed(&sample);

        portENTER_CRITICAL(&s_snap_mux);
        avg_window_push(&s_avg_v, got ? reading.volts : NAN);
        for (int ch = 0; ch < CHANNELS; ch++) {
            avg_window_push(&s_avg_a[ch], got ? reading.amps[ch] : NAN);
            avg_window_push(&s_avg_w[ch], got ? reading.watts[ch] : NAN);
        }
        portEXIT_CRITICAL(&s_snap_mux);

        int64_t total_mwh[CHANNELS];
        for (int ch = 0; ch < CHANNELS; ch++) {
            total_mwh[ch] = energy_total_mwh(ch);
            if (energy_save_due(&s_save[ch], total_mwh[ch], now_ms)) {
#if HAS_PULSE_COUNTER
                nvs_save_energy(ch, total_mwh[ch], s_energy.last_cf);
#else
                nvs_save_energy(ch, total_mwh[ch], 0);
#endif
                energy_save_done(&s_save[ch], total_mwh[ch], now_ms);
            }
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
            s_snap.hz = reading.hz;
            s_snap.raw_v = reading.raw_v;
            for (int ch = 0; ch < CHANNELS; ch++) {
                s_snap.amps[ch] = reading.amps[ch];
                s_snap.watts[ch] = reading.watts[ch];
                s_snap.commanded_on[ch] = relay_get_ch(ch);
                s_snap.total_mwh[ch] = total_mwh[ch];
                s_snap.raw_i[ch] = reading.raw_i[ch];
                s_snap.raw_p[ch] = reading.raw_p[ch];
            }
            portEXIT_CRITICAL(&s_snap_mux);

            if (s_advertised) {
                schedule(publish_work);
                bool notify = now_ms - last_notify_ms >= NOTIFY_INTERVAL_MS;
                for (int ch = 0; ch < CHANNELS; ch++) {
                    notify = notify || total_mwh[ch] - last_notified_mwh[ch] >= NOTIFY_DELTA_MWH;
                }
                if (notify) {
                    for (int ch = 0; ch < CHANNELS; ch++) {
                        last_notified_mwh[ch] = total_mwh[ch];
                    }
                    last_notify_ms = now_ms;
                    schedule(energy_work);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

// ---------------------------------------------------------------------------
// Live readings (Wi-Fi maintenance page)
// ---------------------------------------------------------------------------

bool metering_live(metering_live_t *out)
{
    *out = {};
    out->channels = CHANNELS;
    out->chip = CHIP_NAME;
    shelly_cal_get(&out->ratios);
    portENTER_CRITICAL(&s_snap_mux);
    out->valid = s_snap.valid;
    out->volts = s_snap.volts;
    out->hz = s_snap.hz;
    out->raw_v = s_snap.raw_v;
    for (int ch = 0; ch < CHANNELS; ch++) {
        out->amps[ch] = s_snap.amps[ch];
        out->watts[ch] = s_snap.watts[ch];
        out->relay_on[ch] = s_snap.commanded_on[ch];
        out->energy_mwh[ch] = s_snap.total_mwh[ch];
        out->raw_i[ch] = s_snap.raw_i[ch];
        out->raw_p[ch] = s_snap.raw_p[ch];
    }
    out->steady_ready = avg_window_full(&s_avg_v);
    portEXIT_CRITICAL(&s_snap_mux);
    return true;
}

bool metering_cal_measure(int ch, cal_values_t *mean, cal_values_t *spread)
{
    if (ch < 0 || ch >= CHANNELS) {
        return false;
    }
    mean->volts = mean->amps = mean->watts = NAN;
    spread->volts = spread->amps = spread->watts = NAN;
    portENTER_CRITICAL(&s_snap_mux);
    if (avg_window_full(&s_avg_v) && avg_window_full(&s_avg_a[ch]) && avg_window_full(&s_avg_w[ch])) {
        mean->volts = avg_window_mean(&s_avg_v);
        mean->amps = avg_window_mean(&s_avg_a[ch]);
        mean->watts = avg_window_mean(&s_avg_w[ch]);
        spread->volts = avg_window_spread(&s_avg_v);
        spread->amps = avg_window_spread(&s_avg_a[ch]);
        spread->watts = avg_window_spread(&s_avg_w[ch]);
    }
    portEXIT_CRITICAL(&s_snap_mux);
    return true;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

#if HAS_PULSE_COUNTER
// Poll for up to 3s: is this board on AC mains? Also measures the line
// frequency and reconfigures the meter if it differs from the default.
static bool detect_ac(bool *line_60hz)
{
    ac_detect_t ac;
    ac_detect_init(&ac, AC_DETECT_V, AC_DETECT_COUNT);
    float hz = NAN;

    for (int elapsed = 0; elapsed < AC_DETECT_WINDOW_MS; elapsed += AC_DETECT_SAMPLE_MS) {
        chip_reading_t r;
        if (chip_read(&r)) {
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
#else
// Poll for up to 3s: is this board on AC mains? The ADE7953 needs no line
// frequency setting.
static bool detect_ac(void)
{
    ac_detect_t ac;
    ac_detect_init(&ac, AC_DETECT_V, AC_DETECT_COUNT);
    for (int elapsed = 0; elapsed < AC_DETECT_WINDOW_MS; elapsed += AC_DETECT_SAMPLE_MS) {
        chip_reading_t r;
        if (chip_read(&r) && ac_detect_update(&ac, r.volts)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(AC_DETECT_SAMPLE_MS));
    }
    return ac.present;
}
#endif

esp_err_t metering_init(void)
{
#if HAS_PULSE_COUNTER
    bool line_60hz = true;
#ifndef CONFIG_SHELLY_BL0942_LINE_60HZ
    line_60hz = false;
#endif
    esp_err_t err = bl0942_init(line_60hz);
#else
    esp_err_t err = ade7953_init();
#endif
    if (err != ESP_OK) {
        // Keep going: readings may still arrive, and if not the meter-lost
        // alert is raised. The relays keep working either way.
        ESP_LOGE(TAG, "Meter setup failed: %s", esp_err_to_name(err));
    }

    float base[CAL_Q_COUNT];
    chip_base(base);
    shelly_cal_init(base);

    int64_t total_mwh[CHANNELS] = {};
    uint32_t cf_base = 0;
    bool have_energy = false;
    nvs_load(total_mwh, &cf_base, &have_energy);

#if HAS_PULSE_COUNTER
    bool ac_now = detect_ac(&line_60hz);
#else
    bool ac_now = detect_ac();
#endif
    if (ac_now && !s_ac_seen) {
        s_ac_seen = true;
        nvs_save_ac_seen();
    }
    s_advertised = s_ac_seen;
    ESP_LOGI(TAG, "AC %s now, %s: metering %s", ac_now ? "present" : "not detected",
             s_ac_seen ? "seen before" : "never seen", s_advertised ? "advertised" : "not advertised");

    int64_t now_ms = esp_timer_get_time() / 1000;
#if HAS_PULSE_COUNTER
    s_mwh_per_pulse = bl0942_mwh_per_pulse(bl0942_calibration());
    energy_acc_init(&s_energy, s_mwh_per_pulse, MAX_PLAUSIBLE_W, (double)RESTORE_SLACK_MWH);
    if (have_energy) {
        // After a software reset the meter kept counting: resume from the
        // saved baseline. After a power loss the counter restarted and the
        // first sample reads as a chip reset.
        energy_acc_restore(&s_energy, total_mwh[0], cf_base, now_ms);
    }
#else
    // Integrated from power: what was saved is where counting resumes. Up
    // to the last 100 Wh or 15 minutes before a power cut is lost.
    for (int ch = 0; ch < CHANNELS; ch++) {
        s_energy_mwh[ch] = (double)total_mwh[ch];
    }
#endif
    avg_window_init(&s_avg_v, CAL_WINDOW_POLLS);
    for (int ch = 0; ch < CHANNELS; ch++) {
        energy_save_policy_init(&s_save[ch], SAVE_DELTA_MWH, SAVE_INTERVAL_MS, total_mwh[ch], now_ms);
        s_snap.total_mwh[ch] = total_mwh[ch];
        avg_window_init(&s_avg_a[ch], CAL_WINDOW_POLLS);
        avg_window_init(&s_avg_w[ch], CAL_WINDOW_POLLS);
#if CHANNELS > 1
        ESP_LOGI(TAG, "Relay %d energy total %lld Wh", ch + 1, (long long)(total_mwh[ch] / 1000));
#else
        ESP_LOGI(TAG, "Energy total %lld Wh", (long long)(total_mwh[ch] / 1000));
#endif
    }
    s_snap.hz = NAN;

    if (xTaskCreate(meter_task, "meter", TASK_STACK_SIZE, nullptr, TASK_PRIORITY, nullptr) != pdPASS) {
        // Without the task there is no protection. The breaker still is.
        ESP_LOGE(TAG, "Failed to create the meter task: electrical protection is NOT running");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t metering_add_to_endpoint_ch(int ch, endpoint_t *endpoint)
{
    if (!s_advertised) {
        return ESP_OK;
    }
    if (ch < 0 || ch >= CHANNELS) {
        return ESP_ERR_INVALID_ARG;
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
    s_epm_cluster[ch] = new RegisteredServerCluster<ShellyEpmCluster>(EPM::ElectricalPowerMeasurementCluster::Config{
        .endpointId = endpoint_id,
        .delegate = s_epm_delegate[ch],
        .features = BitMask<EPM::Feature>(EPM::Feature::kAlternatingCurrent),
        .optionalAttributes = optional,
    });
    CHIP_ERROR chip_err = esp_matter::data_model::provider::get_instance().registry().Register(
        s_epm_cluster[ch]->Registration());
    if (chip_err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Failed to register the power measurement cluster: %" CHIP_ERROR_FORMAT, chip_err.Format());
    }

    s_endpoint_id[ch] = endpoint_id;
    ESP_LOGI(TAG, "Metering on endpoint %u", endpoint_id);
    return ESP_OK;
}

esp_err_t metering_add_to_endpoint(endpoint_t *endpoint)
{
    return metering_add_to_endpoint_ch(0, endpoint);
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
    for (int ch = 0; ch < CHANNELS; ch++) {
        if (s_endpoint_id[ch] == 0) {
            continue;
        }
        CHIP_ERROR err = ElectricalEnergyMeasurement::SetMeasurementAccuracy(s_endpoint_id[ch], accuracy);
        if (err != CHIP_NO_ERROR) {
            ESP_LOGW(TAG, "Failed to set the energy accuracy: %" CHIP_ERROR_FORMAT, err.Format());
        }
    }
    // Publish the restored totals now, so CumulativeEnergyImported is not
    // null until the first 5-minute report.
    energy_work(0);
    publish_work(0);
}

void metering_matter_started(void)
{
    if (!s_advertised || s_endpoint_id[0] == 0) {
        return;
    }
    s_matter_ready = true;
    schedule(matter_started_work);
}

#endif // CONFIG_SHELLY_HAS_METER
