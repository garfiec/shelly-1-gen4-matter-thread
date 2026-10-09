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


// calibration_logic.h
//
// The meter calibration entered on the Wi-Fi maintenance page: with a load
// running, the owner types what a reference meter reads, and the device
// works out a correction for each quantity. Pure C, shared by the firmware
// and the host tests.
//
// A correction is a ratio applied on top of the compiled calibration (1.0
// for none). It is stored with the compiled calibration it was made
// against ("base", in units per meter count), so a firmware with better
// compiled values keeps the device's real calibration instead of applying
// the old correction twice (cal_rebase()).
//
// Every ratio stays within CAL_RATIO_MIN..CAL_RATIO_MAX of the compiled
// value: a typo, or a reference meter on the wrong load, can't move the
// readings (or the overcurrent protection, which uses them) further.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAL_CHANNELS        2

// The quantities, in storage order.
enum {
    CAL_Q_V = 0,            // Voltage (one for the device)
    CAL_Q_I0,               // Current, relay 1
    CAL_Q_I1,               // Current, relay 2
    CAL_Q_P0,               // Active power, relay 1
    CAL_Q_P1,               // Active power, relay 2
    CAL_Q_COUNT,
};

#define CAL_Q_I(ch)         (CAL_Q_I0 + (ch))
#define CAL_Q_P(ch)         (CAL_Q_P0 + (ch))

#define CAL_RATIO_MIN       0.85f
#define CAL_RATIO_MAX       1.15f

// The device's own readings must be at least this for that quantity to be
// calibrated: below them the meter's noise and the reference meter's
// resolution dominate.
#define CAL_MIN_VOLTS       80.0f
#define CAL_MIN_AMPS        0.5f
#define CAL_MIN_WATTS       50.0f

// The averaged readings may vary by at most this fraction over the window.
#define CAL_MAX_SPREAD      0.03f

typedef struct {
    float q[CAL_Q_COUNT];
} cal_ratios_t;

void cal_ratios_default(cal_ratios_t *r);
// Every ratio finite and within bounds.
bool cal_ratios_valid(const cal_ratios_t *r);
// All 1.0.
bool cal_ratios_is_default(const cal_ratios_t *r);

// Readings of one relay: from the device (averaged, with the ratios in use
// applied), or typed from a reference meter. NAN for one not given.
typedef struct {
    float volts;
    float amps;
    float watts;
} cal_values_t;

typedef enum {
    CAL_OK = 0,
    CAL_ERR_NOTHING,        // No reference value given
    CAL_ERR_INPUT,          // A value that is not a positive number
    CAL_ERR_CHANNEL,        // No such relay
    CAL_ERR_NO_READING,     // The meter has no current reading
    CAL_ERR_LOW_VOLTAGE,    // Device reads under CAL_MIN_VOLTS
    CAL_ERR_LOW_CURRENT,    // Under CAL_MIN_AMPS
    CAL_ERR_LOW_POWER,      // Under CAL_MIN_WATTS
    CAL_ERR_UNSTABLE,       // The load changed during the averaging window
    CAL_ERR_RANGE,          // The correction would leave the bounds
} cal_result_t;

const char *cal_result_message(cal_result_t r);

// Work out new ratios for relay ch (0-based) of a board with `channels`
// relays. Each reference value given replaces its ratio by
// old * reference / measured. Voltage is the device's, shared by the relays.
// Without a power reference, the power ratio follows the voltage and current
// corrections (power is computed from both). Leaves *out unchanged unless
// the result is CAL_OK.
cal_result_t cal_compute(const cal_ratios_t *current, int channels, int ch, const cal_values_t *measured,
                         bool stable, const cal_values_t *reference, cal_ratios_t *out);

// Ratios stored against base_old (units per count), for a firmware whose
// compiled values are base_new: the same effective calibration, as ratios
// on base_new. A quantity whose result leaves the bounds, or whose base is
// not a positive number, falls back to 1.0. Returns false if any did.
bool cal_rebase(const cal_ratios_t *stored, const float base_old[CAL_Q_COUNT], const float base_new[CAL_Q_COUNT],
                cal_ratios_t *out);

// Parse the page's form: "ch=1&v=117.4&a=3.31&w=389" (ch 1-based; v, a, w
// each optional, empty means not given). False if it is malformed.
bool cal_parse_form(const char *text, int *ch, cal_values_t *reference);

// The stored record: magic, version, base and ratios, and a check.
#define CAL_RECORD_MAGIC    0x4C414353u     // 'SCAL'
#define CAL_RECORD_VERSION  1u

typedef struct {
    uint32_t magic;
    uint32_t version;
    float base[CAL_Q_COUNT];
    float ratio[CAL_Q_COUNT];
    uint32_t check;
} cal_record_t;

void cal_record_make(cal_record_t *rec, const float base[CAL_Q_COUNT], const cal_ratios_t *ratios);
bool cal_record_valid(const cal_record_t *rec);

#ifdef __cplusplus
}
#endif
