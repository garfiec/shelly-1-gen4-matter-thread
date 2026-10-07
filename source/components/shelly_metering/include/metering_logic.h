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

// metering_logic.h
//
// Pure metering logic, shared by the firmware and the host tests. No
// ESP-IDF or Matter dependencies. Times are in milliseconds from any
// monotonic clock.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Energy accumulator
// ---------------------------------------------------------------------------
//
// Turns the BL0942's 24-bit CF_CNT pulse counter into a monotonic total.
//
//  - A decrease of CF_CNT is a chip reset (power loss, brownout), never a
//    wrap: the counter only wraps after ~3409 kWh, and treating a reset as
//    a wrap would add a phantom 3.4 MWh. The delta is dropped and the
//    baseline re-set.
//  - A delta larger than max_w could deliver in the elapsed time is a
//    glitch and is dropped. A delayed poll keeps its real energy, because
//    the cap grows with the elapsed time.
//  - After a software reset (the BL0942 keeps counting) the persisted
//    baseline resumes the count, so energy since the last save is kept.

typedef struct {
    double mwh_per_pulse;
    float max_w;                // Most power a real load can draw
    double slack_mwh;           // Extra allowance on the first delta after a restore
    double total_mwh;           // Running total, fractional
    uint32_t last_cf;
    int64_t last_ms;
    bool have_last;
    bool first_after_restore;
} energy_acc_t;

void energy_acc_init(energy_acc_t *e, double mwh_per_pulse, float max_w, double slack_mwh);

// Resume from a persisted total and CF_CNT baseline. The first delta after
// a restore may include up to slack_mwh (energy counted since the last
// save) on top of the elapsed-time cap.
void energy_acc_restore(energy_acc_t *e, int64_t total_mwh, uint32_t last_cf, int64_t now_ms);

typedef enum {
    ENERGY_ADDED,           // Delta accepted (possibly zero)
    ENERGY_BASELINE,        // First sample; nothing to add yet
    ENERGY_CHIP_RESET,      // CF_CNT went down; delta dropped
    ENERGY_GLITCH,          // Implausible delta; dropped
} energy_result_t;

energy_result_t energy_acc_update(energy_acc_t *e, uint32_t cf_cnt, int64_t now_ms);

int64_t energy_acc_total_mwh(const energy_acc_t *e);

// ---------------------------------------------------------------------------
// Persistence policy
// ---------------------------------------------------------------------------
//
// Save when the total has grown by save_delta_mwh, or save_interval_ms has
// passed with any change. Nothing is written while the total is unchanged.

typedef struct {
    int64_t save_delta_mwh;
    int64_t save_interval_ms;
    int64_t saved_mwh;
    int64_t saved_ms;
} energy_save_policy_t;

void energy_save_policy_init(energy_save_policy_t *p, int64_t save_delta_mwh, int64_t save_interval_ms,
                             int64_t saved_mwh, int64_t now_ms);
bool energy_save_due(const energy_save_policy_t *p, int64_t total_mwh, int64_t now_ms);
void energy_save_done(energy_save_policy_t *p, int64_t total_mwh, int64_t now_ms);

// ---------------------------------------------------------------------------
// Report thresholds
// ---------------------------------------------------------------------------

// True if a new value differs enough from the last reported one: by at
// least abs_step, or rel_step (fraction) of the last value, whichever is
// larger. A NAN on either side reports when the other is not NAN.
bool report_changed(float last, float now, float abs_step, float rel_step);

#ifdef __cplusplus
}
#endif
