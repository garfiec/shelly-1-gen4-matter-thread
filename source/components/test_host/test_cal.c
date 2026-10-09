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


// test_cal.c
//
// Host tests for metering on the 2PM and the calibration page: the ADE7953
// codec (scaling, and the only writes the firmware may send), the shared
// calibration logic (calibration_logic.c), and the metering helpers it
// relies on (no-load floor, energy from power, averaging window).

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ade7953_codec.h"
#include "calibration_logic.h"
#include "metering_logic.h"

static int s_failures = 0;
static int s_checks = 0;

#define CHECK(cond, ...)                \
    do {                                \
        s_checks++;                     \
        if (!(cond)) {                  \
            s_failures++;               \
            printf("FAIL: ");           \
            printf(__VA_ARGS__);        \
            printf("\n");               \
        }                               \
    } while (0)

static int near(float a, float b, float tol)
{
    return fabsf(a - b) <= tol;
}

// The 2PM's calibration (Kconfig defaults), from the bring-up runs.
static void cal_2pm(ade7953_cal_t *c)
{
    c->v_mul = 0.9857f;
    c->i_mul[0] = 3.795f;
    c->i_mul[1] = 3.812f;
    c->p_mul[0] = -3.726f;
    c->p_mul[1] = -3.739f;
}

static void test_ade_codec(void)
{
    // The only writes: the datasheet's unlock and 0x120 setup, with their values.
    CHECK(ade7953_write_allowed(0x0FE, 0xAD), "unlock allowed");
    CHECK(ade7953_write_allowed(0x120, 0x0030), "setup allowed");
    CHECK(!ade7953_write_allowed(0x0FE, 0xAC), "unlock with another value refused");
    CHECK(!ade7953_write_allowed(0x120, 0x0031), "setup with another value refused");
    CHECK(!ade7953_write_allowed(0x380, 0x400000), "a gain register refused");
    CHECK(!ade7953_write_allowed(0x102, 0x8004), "CONFIG refused");
    CHECK(!ade7953_write_allowed(0x007, 0), "PGA refused");

    uint8_t b[4] = {0x00, 0x2F, 0x2F, 0x13};
    CHECK(ade7953_be_value(b, 4) == 0x002F2F13, "big-endian join");
    CHECK(ade7953_be_value(b + 2, 2) == 0x2F13, "16-bit join");

    // Channel A run: Kill-A-Watt 117.35 V, 3.31 A, 389 W (PF 1.00).
    ade7953_cal_t cal;
    cal_2pm(&cal);
    ade7953_raw_t raw = {0};
    raw.vrms = 0x002F2F13;
    raw.irms[0] = 0x000154B1;
    raw.irms[1] = 0x000006ED;
    raw.watt[0] = (int32_t)0xFFFFC134;
    raw.watt[1] = (int32_t)0xFFFFFFEA;
    raw.period = 0x0E90;
    ade7953_reading_t r;
    ade7953_scale(&cal, &raw, &r);
    CHECK(near(r.volts, 117.35f, 0.5f), "channel A run voltage %.2f", r.volts);
    CHECK(near(r.amps[0], 3.31f, 0.02f), "channel A run current %.3f", r.amps[0]);
    CHECK(near(r.watts[0], 389.0f, 2.0f), "channel A run power %.1f (reverse wiring counts positive)", r.watts[0]);
    CHECK(near(r.hz, 60.0f, 0.05f), "frequency %.2f", r.hz);
    CHECK(r.amps[1] < 0.1f && r.watts[1] < 1.0f, "idle channel B reads its noise only");

    // Channel B run: 3.315 A, 389.5 W.
    raw.irms[1] = 0x000153B6;
    raw.watt[1] = (int32_t)0xFFFFC156;
    ade7953_scale(&cal, &raw, &r);
    CHECK(near(r.amps[1], 3.315f, 0.02f), "channel B run current %.3f", r.amps[1]);
    CHECK(near(r.watts[1], 389.5f, 2.0f), "channel B run power %.1f", r.watts[1]);

    raw.period = 0;
    ade7953_scale(&cal, &raw, &r);
    CHECK(isnan(r.hz), "no period, no frequency");
    raw.period = 0xFFFF;
    ade7953_scale(&cal, &raw, &r);
    CHECK(isnan(r.hz), "all-ones period, no frequency");

    CHECK(ade7953_watts_per_count(&cal, 0) > 0.0f, "power base is a magnitude");
}

static cal_values_t vals(float v, float a, float w)
{
    cal_values_t x = {v, a, w};
    return x;
}

static void test_cal_compute(void)
{
    cal_ratios_t cur, out;
    cal_ratios_default(&cur);
    CHECK(cal_ratios_valid(&cur) && cal_ratios_is_default(&cur), "default ratios");

    cal_values_t meas = vals(116.0f, 3.25f, 380.0f);
    cal_values_t none = vals(NAN, NAN, NAN);
    out = cur;
    CHECK(cal_compute(&cur, 2, 0, &meas, true, &none, &out) == CAL_ERR_NOTHING, "nothing given");
    cal_values_t bad = vals(-1.0f, NAN, NAN);
    CHECK(cal_compute(&cur, 2, 0, &meas, true, &bad, &out) == CAL_ERR_INPUT, "negative refused");
    bad = vals(INFINITY, NAN, NAN);
    CHECK(cal_compute(&cur, 2, 0, &meas, true, &bad, &out) == CAL_ERR_INPUT, "infinity refused");
    cal_values_t ref = vals(117.35f, 3.31f, 389.0f);
    CHECK(cal_compute(&cur, 1, 1, &meas, true, &ref, &out) == CAL_ERR_CHANNEL, "relay 2 on a 1-relay board");
    CHECK(cal_compute(&cur, 2, 2, &meas, true, &ref, &out) == CAL_ERR_CHANNEL, "relay 3");
    CHECK(cal_compute(&cur, 2, 0, &meas, false, &ref, &out) == CAL_ERR_UNSTABLE, "unsteady load");
    cal_values_t nomeas = vals(NAN, NAN, NAN);
    CHECK(cal_compute(&cur, 2, 0, &nomeas, true, &ref, &out) == CAL_ERR_NO_READING, "no reading");

    cal_values_t small = vals(116.0f, 0.3f, 35.0f);
    cal_values_t ref_a = vals(NAN, 0.31f, NAN);
    CHECK(cal_compute(&cur, 2, 0, &small, true, &ref_a, &out) == CAL_ERR_LOW_CURRENT, "0.3 A is too small");
    cal_values_t ref_w = vals(NAN, NAN, 36.0f);
    CHECK(cal_compute(&cur, 2, 0, &small, true, &ref_w, &out) == CAL_ERR_LOW_POWER, "35 W is too small");
    cal_values_t dc = vals(12.0f, 3.0f, 36.0f);
    cal_values_t ref_v = vals(12.1f, NAN, NAN);
    CHECK(cal_compute(&cur, 2, 0, &dc, true, &ref_v, &out) == CAL_ERR_LOW_VOLTAGE, "not mains");
    // Voltage only, with no load: nothing about current or power is checked.
    cal_values_t idle = vals(116.0f, 0.0f, 0.0f);
    cal_values_t ref_v2 = vals(117.0f, NAN, NAN);
    CHECK(cal_compute(&cur, 2, 0, &idle, true, &ref_v2, &out) == CAL_OK, "voltage alone with no load");

    // A typo (39 instead of 389) is out of bounds and changes nothing.
    cal_ratios_t keep = cur;
    out = cur;
    cal_values_t typo = vals(NAN, NAN, 39.0f);
    CHECK(cal_compute(&cur, 2, 0, &meas, true, &typo, &out) == CAL_ERR_RANGE, "typo refused");
    CHECK(memcmp(&out, &keep, sizeof(out)) == 0, "output untouched on refusal");

    // All three, relay 2.
    CHECK(cal_compute(&cur, 2, 1, &meas, true, &ref, &out) == CAL_OK, "full calibration");
    CHECK(near(out.q[CAL_Q_V], 117.35f / 116.0f, 1e-5f), "voltage ratio");
    CHECK(near(out.q[CAL_Q_I1], 3.31f / 3.25f, 1e-5f), "relay 2 current ratio");
    CHECK(near(out.q[CAL_Q_P1], 389.0f / 380.0f, 1e-5f), "relay 2 power ratio");
    CHECK(out.q[CAL_Q_I0] == 1.0f && out.q[CAL_Q_P0] == 1.0f, "relay 1 untouched");

    // Builds on the ratios in use: measured values already include them.
    cal_ratios_t cur2 = out, out2;
    cal_values_t meas2 = vals(117.35f, 3.31f, 389.0f);
    CHECK(cal_compute(&cur2, 2, 1, &meas2, true, &ref, &out2) == CAL_OK, "recalibrating on itself");
    CHECK(near(out2.q[CAL_Q_P1], out.q[CAL_Q_P1], 1e-5f), "a match changes nothing");

    // Without a power reference, power follows voltage and current.
    cal_values_t ref_va = vals(117.35f, 3.31f, NAN);
    CHECK(cal_compute(&cur, 2, 0, &meas, true, &ref_va, &out) == CAL_OK, "volts and amps only");
    CHECK(near(out.q[CAL_Q_P0], (117.35f / 116.0f) * (3.31f / 3.25f), 1e-5f), "power follows V and I");
}

static void test_cal_rebase_and_form(void)
{
    cal_ratios_t stored, out;
    cal_ratios_default(&stored);
    stored.q[CAL_Q_V] = 1.02f;
    stored.q[CAL_Q_I0] = 0.98f;
    float base[CAL_Q_COUNT] = {1e-4f, 3e-5f, 3e-5f, 0.02f, 0.02f};
    CHECK(cal_rebase(&stored, base, base, &out) && memcmp(&out, &stored, sizeof(out)) == 0, "same base, same ratios");

    // Better compiled values (2% higher voltage): the ratio shrinks to match.
    float base2[CAL_Q_COUNT] = {1.02e-4f, 3e-5f, 3e-5f, 0.02f, 0.02f};
    CHECK(cal_rebase(&stored, base, base2, &out), "rebase in bounds");
    CHECK(near(out.q[CAL_Q_V], 1.0f, 1e-5f), "same effective voltage calibration");
    CHECK(near(out.q[CAL_Q_I0], 0.98f, 1e-6f), "current unchanged");

    // A rebase out of bounds falls back to the built-in value.
    float base3[CAL_Q_COUNT] = {0.5e-4f, 3e-5f, 3e-5f, 0.02f, 0.02f};
    CHECK(!cal_rebase(&stored, base, base3, &out) && out.q[CAL_Q_V] == 1.0f, "out of bounds falls back");
    float base0[CAL_Q_COUNT] = {0.0f, 3e-5f, 3e-5f, 0.02f, 0.02f};
    CHECK(!cal_rebase(&stored, base0, base, &out) && out.q[CAL_Q_V] == 1.0f, "a zero base falls back");

    int ch;
    cal_values_t ref;
    CHECK(cal_parse_form("ch=1&v=117.4&a=3.31&w=389", &ch, &ref) && ch == 0 && near(ref.volts, 117.4f, 1e-4f) &&
              near(ref.amps, 3.31f, 1e-5f) && near(ref.watts, 389.0f, 1e-3f),
          "full form");
    CHECK(cal_parse_form("ch=2&v=&a=&w=389", &ch, &ref) && ch == 1 && isnan(ref.volts) && isnan(ref.amps) &&
              near(ref.watts, 389.0f, 1e-3f),
          "empty fields are not given");
    CHECK(!cal_parse_form("v=117", &ch, &ref), "relay missing");
    CHECK(!cal_parse_form("ch=1&v=abc", &ch, &ref), "not a number");
    CHECK(!cal_parse_form("ch=1&x=1", &ch, &ref), "unknown field");
    CHECK(!cal_parse_form("ch=0&v=117", &ch, &ref), "relay 0");
    CHECK(!cal_parse_form("ch=1.5&v=117", &ch, &ref), "fractional relay");
    CHECK(!cal_parse_form("ch=1&v", &ch, &ref), "field without =");
    CHECK(!cal_parse_form("ch=1&v=117.4x", &ch, &ref), "trailing junk");

    cal_record_t rec;
    cal_record_make(&rec, base, &stored);
    CHECK(cal_record_valid(&rec), "record valid");
    rec.ratio[0] = 1.03f;
    CHECK(!cal_record_valid(&rec), "tampered record refused");
    cal_ratios_t wild = stored;
    wild.q[CAL_Q_P1] = 2.0f;
    cal_record_make(&rec, base, &wild);
    CHECK(!cal_record_valid(&rec), "out-of-bounds record refused");
}

static void test_meter_helpers(void)
{
    float a = 0.067f, w = 0.4f;
    noload_cut(&a, &w, 1.0f, 0.15f);
    CHECK(a == 0.0f && w == 0.0f, "idle noise reads 0");
    a = 0.06f;
    w = 4.0f;
    noload_cut(&a, &w, 1.0f, 0.15f);
    CHECK(a == 0.06f && w == 4.0f, "a 4 W LED bulb keeps its reading");
    a = 0.5f;
    w = 0.5f;
    noload_cut(&a, &w, 1.0f, 0.15f);
    CHECK(a == 0.5f, "a reactive load keeps its current");

    CHECK(near((float)power_to_mwh(3600.0f, 1000, 5000), 1000.0f, 1e-3f), "3600 W for 1 s is 1 Wh");
    CHECK(near((float)power_to_mwh(100.0f, 60000, 5000), (float)(100.0 * 5000 / 3600.0), 1e-3f), "a stall is capped");
    CHECK(power_to_mwh(0.0f, 1000, 5000) == 0.0 && power_to_mwh(NAN, 1000, 5000) == 0.0, "no power, no energy");
    CHECK(power_to_mwh(100.0f, -5, 5000) == 0.0, "time going back adds nothing");

    avg_window_t win;
    avg_window_init(&win, 5);
    for (int i = 0; i < 4; i++) {
        avg_window_push(&win, 100.0f);
    }
    CHECK(!avg_window_full(&win), "not full after 4 of 5");
    avg_window_push(&win, 102.0f);
    CHECK(avg_window_full(&win) && near(avg_window_mean(&win), 100.4f, 1e-4f), "mean");
    CHECK(near(avg_window_spread(&win), 2.0f / 100.4f, 1e-5f), "spread");
    avg_window_push(&win, NAN);
    CHECK(!avg_window_full(&win) && isnan(avg_window_mean(&win)), "a missed reading empties the window");
    avg_window_init(&win, 3);
    for (int i = 0; i < 3; i++) {
        avg_window_push(&win, 0.0f);
    }
    CHECK(isinf(avg_window_spread(&win)), "zero mean has no spread");
    avg_window_init(&win, 100);
    CHECK(win.size == AVG_WINDOW_MAX, "size capped");
}

int main(void)
{
    test_ade_codec();
    test_cal_compute();
    test_cal_rebase_and_form();
    test_meter_helpers();
    if (s_failures) {
        printf("%d of %d calibration checks FAILED\n", s_failures, s_checks);
        return 1;
    }
    printf("All %d calibration checks passed\n", s_checks);
    return 0;
}
