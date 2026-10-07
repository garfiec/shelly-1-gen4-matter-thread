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

// protection_logic.h
//
// Pure electrical-protection state machines, shared by the firmware and
// the host tests. No ESP-IDF or Matter dependencies. Times are in
// milliseconds from any monotonic clock.
//
// This is SECONDARY protection. The home's circuit breaker is the primary
// protection; these functions only open the relay earlier than a breaker
// would, or raise an alert.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Overcurrent: I²t heat model
// ---------------------------------------------------------------------------
//
// Heat builds only above the pickup current:  E += (I² − Ipickup²)·dt.
// At or below the pickup it decays exponentially: E *= exp(−dt/tau).
// The relay trips when E reaches k. A load at or below the pickup never
// trips; short inrush adds little heat, because (I² − Ipickup²)·dt is small
// for a short dt. Heat is NOT cleared by a trip or a reset of the latch,
// so switching straight back into an overload trips sooner.

typedef struct {
    float pickup_a;     // Heat builds above this current
    float k_a2s;        // Trip threshold (A²s)
    float tau_s;        // Decay time constant below the pickup
    float heat_a2s;     // Accumulated heat
} i2t_t;

void i2t_init(i2t_t *m, float pickup_a, float k_a2s, float tau_s);

// Feed one current sample held for dt_s seconds. Returns true while the
// accumulated heat is at or above the trip threshold.
bool i2t_update(i2t_t *m, float amps, float dt_s);

// Seconds to trip from zero heat at a constant current, or a negative
// value if that current never trips. Used for logs and tests.
float i2t_trip_time_s(const i2t_t *m, float amps);

// ---------------------------------------------------------------------------
// Nominal voltage and overvoltage
// ---------------------------------------------------------------------------

// The mains nominal, learned once from settled readings and then stored.
typedef enum {
    MAINS_NOMINAL_UNKNOWN = 0,
    MAINS_NOMINAL_120V = 120,
    MAINS_NOMINAL_240V = 240,
} mains_nominal_t;

typedef struct {
    float present_v;            // Readings at or above this count as mains present
    int settle_updates;         // Consecutive present readings before classifying
    float split_v;              // Below this, a 120 V system; at or above, 240 V
    int present_count;
} nominal_classifier_t;

void nominal_classifier_init(nominal_classifier_t *c, float present_v, int settle_updates, float split_v);

// Feed one RMS update. Returns the classified nominal once enough settled
// readings have arrived, otherwise MAINS_NOMINAL_UNKNOWN. The caller stores
// the first classification and stops feeding: a later surge or a lost
// neutral must never reclassify it.
mains_nominal_t nominal_classifier_update(nominal_classifier_t *c, float volts);

typedef struct {
    float limit_v;              // Trip above this
    int64_t sustain_ms;         // ...for this long
    int64_t recover_ms;         // Recover after this long at or below the limit
    int64_t over_since_ms;      // -1 while not above the limit
    int64_t ok_since_ms;        // -1 while above the limit
    bool tripped;
} overvoltage_t;

void overvoltage_init(overvoltage_t *o, float limit_v, int64_t sustain_ms, int64_t recover_ms);

// The limit for a nominal: 150 V on a 120 V system, 264 V on 240 V.
// Returns 0 for an unknown nominal (no overvoltage protection yet).
float overvoltage_limit_for(mains_nominal_t nominal);

typedef enum {
    OV_EVENT_NONE,
    OV_EVENT_TRIP,
    OV_EVENT_RECOVER,
} ov_event_t;

ov_event_t overvoltage_update(overvoltage_t *o, float volts, int64_t now_ms);

// ---------------------------------------------------------------------------
// Welded relay
// ---------------------------------------------------------------------------
//
// Commanded off for at least off_settle_ms, yet the current stays at or
// above alarm_a for alarm_ms: the contacts did not open. Alert only; the
// firmware cannot open a welded relay. Clears after the current stays
// below clear_a for clear_ms while commanded off, or on the next On/Off
// cycle (the caller resets it on a commanded On).

typedef struct {
    float alarm_a;
    float clear_a;
    int64_t off_settle_ms;
    int64_t alarm_ms;
    int64_t clear_ms;
    int64_t off_since_ms;       // -1 while commanded on
    int64_t high_since_ms;      // -1 while below alarm_a
    int64_t low_since_ms;       // -1 while at or above clear_a
    bool alarmed;
} welded_t;

void welded_init(welded_t *w, float alarm_a, float clear_a, int64_t off_settle_ms, int64_t alarm_ms, int64_t clear_ms);

// Feed one sample. commanded_on is the relay's commanded state. Returns
// true if the alarm state changed.
bool welded_update(welded_t *w, bool commanded_on, float amps, int64_t now_ms);

// ---------------------------------------------------------------------------
// AC presence (decides whether metering is advertised)
// ---------------------------------------------------------------------------
//
// AC is present when V_RMS reaches min_v on required consecutive updates.
// A DC supply reads about 0 V through the BL0942's high-pass filter. The
// FREQ register is not used: its reset value is a genuine 50.00 Hz reading.

typedef struct {
    float min_v;
    int required;
    int count;
    bool present;
} ac_detect_t;

void ac_detect_init(ac_detect_t *a, float min_v, int required);
bool ac_detect_update(ac_detect_t *a, float volts);

#ifdef __cplusplus
}
#endif
