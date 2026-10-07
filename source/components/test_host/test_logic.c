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

// test_logic.c
//
// Host tests for the pure logic in the shared components: the BL0942
// codec, energy accounting, electrical protection, thermal and the fault
// registry mapping. Build and run with test_host/run.sh.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bl0942_codec.h"
#include "metering_logic.h"
#include "protection_logic.h"
#include "shelly_fault_logic.h"
#include "thermal_logic.h"

static int s_failures = 0;
static int s_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        s_checks++;                                                              \
        if (!(cond)) {                                                           \
            s_failures++;                                                        \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
        }                                                                        \
    } while (0)

#define CHECK_NEAR(a, b, tol) CHECK(fabs((double)(a) - (double)(b)) <= (tol))

// Calibration measured on the 1PM Gen4 (Kconfig defaults).
#define V_REF 14462.09548f
#define I_REF 253772.51527f

// ---------------------------------------------------------------------------
// BL0942 codec
// ---------------------------------------------------------------------------

static void put24(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
}

static void make_frame(uint8_t *f, uint32_t i_rms, uint32_t v_rms, int32_t watt, uint32_t cf, uint16_t freq)
{
    memset(f, 0, BL0942_FRAME_LEN);
    f[0] = 0x55;
    put24(&f[1], i_rms);
    put24(&f[4], v_rms);
    put24(&f[7], i_rms);
    put24(&f[10], (uint32_t)watt & 0xFFFFFF);
    put24(&f[13], cf);
    f[16] = freq & 0xFF;
    f[17] = freq >> 8;
    uint8_t sum = 0x58;
    for (int i = 0; i < BL0942_FRAME_LEN - 1; i++) {
        sum += f[i];
    }
    f[BL0942_FRAME_LEN - 1] = (uint8_t)~sum;
}

static void test_bl0942(void)
{
    bl0942_cal_t cal;
    bl0942_cal_init(&cal, V_REF, I_REF, 29.7f);
    // One CF pulse is about 0.2032 Wh with this board's references.
    CHECK_NEAR(cal.pulses_per_kwh, 4921.6, 1.0);
    CHECK_NEAR(bl0942_mwh_per_pulse(&cal), 203.2, 0.1);

    uint8_t f[BL0942_FRAME_LEN];
    uint32_t i_raw = (uint32_t)(10.0f * I_REF);
    uint32_t v_raw = (uint32_t)(120.0f * V_REF);
    int32_t w_raw = (int32_t)(-1200.0f * cal.power_ref);   // reverse power
    make_frame(f, i_raw, v_raw, w_raw, 123456, 16667);      // 60 Hz

    bl0942_raw_t raw;
    CHECK(bl0942_parse_frame(f, sizeof(f), &raw) == BL0942_FRAME_OK);
    CHECK(raw.cf_cnt == 123456);
    CHECK(raw.watt < 0);

    bl0942_reading_t r;
    bl0942_scale(&cal, &raw, &r);
    CHECK_NEAR(r.volts, 120.0, 0.01);
    CHECK_NEAR(r.amps, 10.0, 0.001);
    CHECK_NEAR(r.watts, 1200.0, 0.5);   // absolute: reverse power counts as import
    CHECK_NEAR(r.hz, 60.0, 0.01);
    CHECK(!r.amps_clipped);

    // The FREQ reset value decodes as exactly 50.00 Hz: it cannot tell AC
    // from DC, which is why AC detection uses V_RMS only.
    make_frame(f, 0, 0, 0, 0, BL0942_FREQ_RESET_VALUE);
    CHECK(bl0942_parse_frame(f, sizeof(f), &raw) == BL0942_FRAME_OK);
    bl0942_scale(&cal, &raw, &r);
    CHECK_NEAR(r.hz, 50.0, 0.001);
    raw.freq = 0;
    bl0942_scale(&cal, &raw, &r);
    CHECK(isnan(r.hz));

    // Clipping near the current channel full scale.
    make_frame(f, (uint32_t)(29.0f * I_REF), v_raw, 0, 0, 16667);
    CHECK(bl0942_parse_frame(f, sizeof(f), &raw) == BL0942_FRAME_OK);
    bl0942_scale(&cal, &raw, &r);
    CHECK(r.amps_clipped);

    // Rejections.
    make_frame(f, i_raw, v_raw, 0, 0, 16667);
    f[5] ^= 0x01;
    CHECK(bl0942_parse_frame(f, sizeof(f), &raw) == BL0942_FRAME_BAD_CHECKSUM);
    make_frame(f, i_raw, v_raw, 0, 0, 16667);
    f[0] = 0x54;
    CHECK(bl0942_parse_frame(f, sizeof(f), &raw) == BL0942_FRAME_BAD_HEADER);
    CHECK(bl0942_parse_frame(f, BL0942_FRAME_LEN - 1, &raw) == BL0942_FRAME_SHORT);

    // Register write and read-back checksums.
    uint8_t w[6];
    bl0942_build_write(BL0942_REG_MODE, bl0942_mode_value(true), w);
    CHECK(w[0] == 0xA8 && w[1] == 0x19);
    CHECK((uint8_t)(w[0] + w[1] + w[2] + w[3] + w[4] + w[5]) == 0xFF);
    uint8_t resp[4] = {0xAF, 0x00, 0x00, 0};
    resp[3] = (uint8_t)~(0x58 + 0x19 + 0xAF);
    uint32_t val = 0;
    CHECK(bl0942_parse_reg_response(0x19, resp, &val) && val == 0xAF);
    resp[3] ^= 1;
    CHECK(!bl0942_parse_reg_response(0x19, resp, &val));

    // MODE: CF enabled, absolute energy, 800ms RMS, 60 Hz bit.
    CHECK(bl0942_mode_value(false) == 0x8F);
    CHECK(bl0942_mode_value(true) == 0xAF);
}

// ---------------------------------------------------------------------------
// Energy
// ---------------------------------------------------------------------------

static void test_energy(void)
{
    const double per_pulse = 203.2;
    energy_acc_t e;
    energy_acc_init(&e, per_pulse, 5000.0f, 100000.0);

    CHECK(energy_acc_update(&e, 1000, 0) == ENERGY_BASELINE);
    CHECK(energy_acc_total_mwh(&e) == 0);

    // 1 kW for 1 hour is about 4921 pulses.
    CHECK(energy_acc_update(&e, 1000 + 4921, 3600000) == ENERGY_ADDED);
    CHECK_NEAR(energy_acc_total_mwh(&e), 1000000, 300);

    // A decrease is a chip reset, never a wrap: no phantom 3.4 MWh.
    int64_t before = energy_acc_total_mwh(&e);
    CHECK(energy_acc_update(&e, 5, 3600200) == ENERGY_CHIP_RESET);
    CHECK(energy_acc_total_mwh(&e) == before);
    // Counting resumes from the new baseline.
    CHECK(energy_acc_update(&e, 15, 3700000) == ENERGY_ADDED);
    CHECK_NEAR(energy_acc_total_mwh(&e) - before, 10 * per_pulse, 1);

    // A jump no load could produce in the elapsed time is dropped...
    before = energy_acc_total_mwh(&e);
    CHECK(energy_acc_update(&e, 15 + 100000, 3700200) == ENERGY_GLITCH);
    CHECK(energy_acc_total_mwh(&e) == before);
    // ...but a delayed poll keeps its real energy: 3 kW for 10s.
    uint32_t cf = 15 + 100000;
    uint32_t pulses = (uint32_t)(3000.0 * 10.0 / 3600.0 * 1000.0 / per_pulse);
    CHECK(energy_acc_update(&e, cf + pulses, 3710200) == ENERGY_ADDED);
    CHECK(energy_acc_total_mwh(&e) > before);

    // Restore after a software reset: the meter kept counting, and energy
    // since the last save (up to the slack) is kept.
    energy_acc_restore(&e, 5000000, 20000, 0);
    CHECK(energy_acc_update(&e, 20000 + 400, 200) == ENERGY_ADDED);   // ~81 Wh
    CHECK_NEAR(energy_acc_total_mwh(&e), 5000000 + 400 * per_pulse, 1);
    // Restore after a power loss: the counter restarted.
    energy_acc_restore(&e, 5000000, 20000, 0);
    CHECK(energy_acc_update(&e, 3, 200) == ENERGY_CHIP_RESET);
    CHECK(energy_acc_total_mwh(&e) == 5000000);

    // Save policy: never while unchanged; at 100 Wh or 15 min otherwise.
    energy_save_policy_t p;
    energy_save_policy_init(&p, 100000, 900000, 0, 0);
    CHECK(!energy_save_due(&p, 0, 10000000));
    CHECK(!energy_save_due(&p, 99999, 1000));
    CHECK(energy_save_due(&p, 100000, 1000));
    CHECK(energy_save_due(&p, 1, 900000));
    energy_save_done(&p, 1, 900000);
    CHECK(!energy_save_due(&p, 1, 2000000));
}

// ---------------------------------------------------------------------------
// Overcurrent
// ---------------------------------------------------------------------------

static float run_until_trip(i2t_t *m, float amps, float limit_s)
{
    const float dt = 0.2f;
    for (float t = dt; t <= limit_s; t += dt) {
        if (i2t_update(m, amps, dt)) {
            return t;
        }
    }
    return -1.0f;
}

static void test_i2t(void)
{
    i2t_t m;
    const float pickup = 15.0f * 1.10f;

    // A load at or below the pickup never trips.
    i2t_init(&m, pickup, 2800.0f, 300.0f);
    CHECK(run_until_trip(&m, 15.0f, 24 * 3600.0f) < 0);
    i2t_init(&m, pickup, 2800.0f, 300.0f);
    CHECK(run_until_trip(&m, 16.5f, 3600.0f) < 0);

    // Trip times from the plan.
    i2t_init(&m, pickup, 2800.0f, 300.0f);
    CHECK_NEAR(run_until_trip(&m, 18.0f, 120.0f), 54.1, 0.5);
    i2t_init(&m, pickup, 2800.0f, 300.0f);
    CHECK_NEAR(run_until_trip(&m, 22.5f, 60.0f), 12.0, 0.3);
    i2t_init(&m, pickup, 2800.0f, 300.0f);
    CHECK_NEAR(run_until_trip(&m, 29.7f, 60.0f), 4.6, 0.3);
    CHECK_NEAR(i2t_trip_time_s(&m, 18.0f), 54.1, 0.1);
    CHECK(i2t_trip_time_s(&m, 16.0f) < 0);

    // Inrush: 60 A for 100ms adds a few hundred A²s, far from a trip.
    i2t_init(&m, pickup, 2800.0f, 300.0f);
    CHECK(!i2t_update(&m, 60.0f, 0.1f));
    CHECK(m.heat_a2s > 300.0f && m.heat_a2s < 400.0f);

    // A cycling compressor, as the meter sees it: the current channel clips
    // at 29.7 A, so a start reads at most 29.7 A. A 1s start every 5 minutes
    // with an 8 A running load never accumulates to a trip...
    i2t_init(&m, pickup, 2800.0f, 300.0f);
    bool tripped = false;
    for (int cycle = 0; cycle < 100 && !tripped; cycle++) {
        for (int i = 0; i < 5 && !tripped; i++) {
            tripped |= i2t_update(&m, 29.7f, 0.2f);
        }
        for (int i = 0; i < 1495 && !tripped; i++) {
            tripped |= i2t_update(&m, 8.0f, 0.2f);
        }
    }
    CHECK(!tripped);
    // ...but the same start every 30s (a short-cycling fault) does add up:
    // the model cools with a 5-minute time constant, not instantly.
    i2t_init(&m, pickup, 2800.0f, 300.0f);
    tripped = false;
    for (int cycle = 0; cycle < 100 && !tripped; cycle++) {
        for (int i = 0; i < 5 && !tripped; i++) {
            tripped |= i2t_update(&m, 29.7f, 0.2f);
        }
        for (int i = 0; i < 145 && !tripped; i++) {
            tripped |= i2t_update(&m, 8.0f, 0.2f);
        }
    }
    CHECK(tripped);

    // Heat is kept after a trip: switching straight back in trips sooner.
    i2t_init(&m, pickup, 2800.0f, 300.0f);
    CHECK(run_until_trip(&m, 22.5f, 60.0f) > 0);
    i2t_update(&m, 0.0f, 2.0f);   // 2s off (the cooldown)
    float again = run_until_trip(&m, 22.5f, 60.0f);
    CHECK(again > 0 && again < 1.0f);

    // Cooling: after 5 time constants below the pickup, about 1% remains.
    i2t_init(&m, pickup, 2800.0f, 300.0f);
    m.heat_a2s = 2000.0f;
    for (int i = 0; i < 1500 * 5; i++) {
        i2t_update(&m, 5.0f, 0.2f);
    }
    CHECK_NEAR(m.heat_a2s, 2000.0 * exp(-5.0), 2.0);
}

// ---------------------------------------------------------------------------
// Nominal voltage and overvoltage
// ---------------------------------------------------------------------------

static void test_overvoltage(void)
{
    nominal_classifier_t c;
    nominal_classifier_init(&c, 80.0f, 3, 180.0f);
    // The BL0942 reads 0 V before its first RMS update after power-up.
    CHECK(nominal_classifier_update(&c, 0.0f) == MAINS_NOMINAL_UNKNOWN);
    CHECK(nominal_classifier_update(&c, 121.0f) == MAINS_NOMINAL_UNKNOWN);
    CHECK(nominal_classifier_update(&c, 0.0f) == MAINS_NOMINAL_UNKNOWN);  // restarts
    CHECK(nominal_classifier_update(&c, 121.0f) == MAINS_NOMINAL_UNKNOWN);
    CHECK(nominal_classifier_update(&c, 120.0f) == MAINS_NOMINAL_UNKNOWN);
    CHECK(nominal_classifier_update(&c, 119.0f) == MAINS_NOMINAL_120V);
    nominal_classifier_init(&c, 80.0f, 3, 180.0f);
    nominal_classifier_update(&c, 230.0f);
    nominal_classifier_update(&c, 231.0f);
    CHECK(nominal_classifier_update(&c, 229.0f) == MAINS_NOMINAL_240V);

    CHECK(overvoltage_limit_for(MAINS_NOMINAL_120V) == 150.0f);
    CHECK(overvoltage_limit_for(MAINS_NOMINAL_240V) == 264.0f);
    CHECK(overvoltage_limit_for(MAINS_NOMINAL_UNKNOWN) == 0.0f);

    overvoltage_t o;
    // Unknown nominal: no protection yet.
    overvoltage_init(&o, 0.0f, 5000, 30000);
    CHECK(overvoltage_update(&o, 400.0f, 0) == OV_EVENT_NONE);

    overvoltage_init(&o, 150.0f, 5000, 30000);
    // A 4s surge does not trip.
    for (int64_t t = 0; t <= 4000; t += 200) {
        CHECK(overvoltage_update(&o, 200.0f, t) == OV_EVENT_NONE);
    }
    CHECK(overvoltage_update(&o, 120.0f, 4200) == OV_EVENT_NONE);
    // A sustained 5s one does.
    ov_event_t ev = OV_EVENT_NONE;
    int64_t t = 5000;
    for (; t <= 12000 && ev == OV_EVENT_NONE; t += 200) {
        ev = overvoltage_update(&o, 200.0f, t);
    }
    CHECK(ev == OV_EVENT_TRIP);
    CHECK_NEAR(t - 200 - 5000, 5000, 1);
    // Recovery only after 30s back in range, and a relapse restarts it.
    int64_t back = t;
    CHECK(overvoltage_update(&o, 120.0f, back) == OV_EVENT_NONE);
    CHECK(overvoltage_update(&o, 120.0f, back + 20000) == OV_EVENT_NONE);
    CHECK(overvoltage_update(&o, 160.0f, back + 20200) == OV_EVENT_NONE);
    CHECK(overvoltage_update(&o, 120.0f, back + 20400) == OV_EVENT_NONE);
    CHECK(overvoltage_update(&o, 120.0f, back + 45000) == OV_EVENT_NONE);
    CHECK(overvoltage_update(&o, 120.0f, back + 50400) == OV_EVENT_RECOVER);
    CHECK(!o.tripped);
}

// ---------------------------------------------------------------------------
// Welded relay and AC detection
// ---------------------------------------------------------------------------

static void test_welded(void)
{
    welded_t w;
    welded_init(&w, 0.5f, 0.2f, 2000, 5000, 10000);
    // Load current while commanded on is normal.
    CHECK(!welded_update(&w, true, 10.0f, 0));
    // Commanded off: current decays during the first 2s; no alarm.
    CHECK(!welded_update(&w, false, 3.0f, 100));
    CHECK(!welded_update(&w, false, 0.0f, 1500));
    CHECK(!w.alarmed);
    // Current that keeps flowing well after off alarms after 5s.
    bool changed = false;
    for (int64_t t = 2100; t <= 8000; t += 200) {
        changed |= welded_update(&w, false, 2.0f, t);
    }
    CHECK(changed && w.alarmed);
    // Clears after 10s below 0.2 A while still commanded off.
    for (int64_t t = 8200; t <= 17000; t += 200) {
        welded_update(&w, false, 0.1f, t);
    }
    CHECK(w.alarmed);
    welded_update(&w, false, 0.1f, 18400);
    CHECK(!w.alarmed);
    // Or on the next On command.
    for (int64_t t = 20000; t <= 30000; t += 200) {
        welded_update(&w, false, 2.0f, t);
    }
    CHECK(w.alarmed);
    CHECK(welded_update(&w, true, 2.0f, 30200));
    CHECK(!w.alarmed);

    ac_detect_t a;
    ac_detect_init(&a, 80.0f, 2);
    // DC supply: the BL0942's high-pass filter reads about 0 V.
    for (int i = 0; i < 10; i++) {
        CHECK(!ac_detect_update(&a, 0.4f));
    }
    // One reading is not enough; two in a row are.
    CHECK(!ac_detect_update(&a, 120.0f));
    CHECK(!ac_detect_update(&a, 3.0f));
    CHECK(!ac_detect_update(&a, 120.0f));
    CHECK(ac_detect_update(&a, 119.0f));
    // Once seen, it stays seen.
    CHECK(ac_detect_update(&a, 0.0f));
}

// ---------------------------------------------------------------------------
// Thermal
// ---------------------------------------------------------------------------

static void test_thermal(void)
{
    const ntc_params_t p = {
        .r_fixed_ohm = 10000.0f, .r_nominal_ohm = 10000.0f, .t_nominal_c = 25.0f, .beta = 3350.0f, .vref_mv = 3300.0f};
    float t = 0;
    // 25 °C: NTC equals the fixed resistor, mid-supply.
    CHECK(ntc_temperature_c(&p, 1650.0f, &t) == NTC_READING_OK);
    CHECK_NEAR(t, 25.0, 0.1);
    // Hot is LOW voltage: about 0.39 V at 90 °C.
    CHECK(ntc_temperature_c(&p, 390.0f, &t) == NTC_READING_OK);
    CHECK_NEAR(t, 90.0, 1.0);
    // The low rail (a short) reads as very hot, never as a sensor fault.
    CHECK(ntc_temperature_c(&p, 0.0f, &t) == NTC_READING_OK);
    CHECK(t >= 150.0f);
    CHECK(ntc_temperature_c(&p, 20.0f, &t) == NTC_READING_OK);
    CHECK(t > 90.0f);
    // The high rail is an open circuit.
    CHECK(ntc_temperature_c(&p, 3300.0f, &t) == NTC_READING_OPEN);
    CHECK(ntc_temperature_c(&p, 3200.0f, &t) == NTC_READING_OPEN);
    CHECK(ntc_temperature_c(&p, 3000.0f, &t) == NTC_READING_OK);

    thermal_hyst_t h;
    thermal_hyst_init(&h, 90.0f, 75.0f, 10000);
    CHECK(!thermal_hyst_update(&h, 95.0f, 0));
    CHECK(!thermal_hyst_update(&h, 95.0f, 9000));
    // A dip below the trip restarts the timer.
    CHECK(!thermal_hyst_update(&h, 89.0f, 9500));
    CHECK(!thermal_hyst_update(&h, 95.0f, 10000));
    CHECK(!thermal_hyst_update(&h, 95.0f, 19000));
    CHECK(thermal_hyst_update(&h, 95.0f, 20000) && h.tripped);
    // Holds between clear and trip.
    CHECK(!thermal_hyst_update(&h, 80.0f, 21000) && h.tripped);
    CHECK(!thermal_hyst_update(&h, 75.0f, 22000) && h.tripped);
    CHECK(thermal_hyst_update(&h, 74.9f, 23000) && !h.tripped);
}

// ---------------------------------------------------------------------------
// Fault registry mapping and report thresholds
// ---------------------------------------------------------------------------

static void test_faults(void)
{
    uint8_t list[11];
    CHECK(shelly_fault_mask_to_list(0, list, 11) == 0);

    // Overcurrent and welded relay share Unspecified; meter lost and NTC
    // open share Sensor. The list is de-duplicated and sorted.
    uint32_t mask = (1u << SHELLY_FAULT_SRC_OVERCURRENT) | (1u << SHELLY_FAULT_SRC_WELDED_RELAY) |
                    (1u << SHELLY_FAULT_SRC_METER_LOST) | (1u << SHELLY_FAULT_SRC_NTC_OPEN);
    size_t n = shelly_fault_mask_to_list(mask, list, 11);
    CHECK(n == 2 && list[0] == SHELLY_HW_FAULT_UNSPECIFIED && list[1] == SHELLY_HW_FAULT_SENSOR);

    // An enum stays while any of its sources is active.
    mask &= ~(1u << SHELLY_FAULT_SRC_METER_LOST);
    n = shelly_fault_mask_to_list(mask, list, 11);
    CHECK(n == 2 && list[1] == SHELLY_HW_FAULT_SENSOR);

    mask = (1u << SHELLY_FAULT_SRC_OVERTEMP) | (1u << SHELLY_FAULT_SRC_OVERVOLTAGE);
    n = shelly_fault_mask_to_list(mask, list, 11);
    CHECK(n == 2 && list[0] == SHELLY_HW_FAULT_RESETTABLE_OVER_TEMP && list[1] == SHELLY_HW_FAULT_POWER_SOURCE);

    CHECK(report_changed(100.0f, 101.0f, 1.0f, 0.02f) == false);  // 2% of 100 W is 2 W
    CHECK(report_changed(100.0f, 102.0f, 1.0f, 0.02f) == true);
    CHECK(report_changed(10.0f, 11.0f, 1.0f, 0.02f) == true);     // 1 W floor
    CHECK(report_changed(NAN, 1.0f, 1.0f, 0.0f) == true);
    CHECK(report_changed(1.0f, NAN, 1.0f, 0.0f) == true);
    CHECK(report_changed(NAN, NAN, 1.0f, 0.0f) == false);
}

int main(void)
{
    test_bl0942();
    test_energy();
    test_i2t();
    test_overvoltage();
    test_welded();
    test_thermal();
    test_faults();
    printf("%d checks, %d failures\n", s_checks, s_failures);
    return s_failures == 0 ? 0 : 1;
}
