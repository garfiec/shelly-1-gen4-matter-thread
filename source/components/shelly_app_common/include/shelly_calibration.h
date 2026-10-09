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


// shelly_calibration.h
//
// The meter calibration entered on the Wi-Fi maintenance page
// (calibration_logic.h), kept in NVS beside the energy totals, so a factory
// reset keeps it: it describes the hardware, not the Matter pairing.
// Only on boards with a meter (CONFIG_SHELLY_HAS_METER).

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "calibration_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

// Called by metering at init with its compiled calibration in units per
// meter count (1.0 for a quantity the board doesn't have). Loads the saved
// ratios, carried over to these compiled values (cal_rebase()).
void shelly_cal_init(const float base[CAL_Q_COUNT]);

// True once shelly_cal_init() ran: this board has a meter to calibrate.
bool shelly_cal_available(void);

// The ratios in use (all 1.0 if none are saved).
void shelly_cal_get(cal_ratios_t *out);

// Counts every change of the ratios, so the meter can notice one.
uint32_t shelly_cal_generation(void);

// Save and apply. Only from shelly_change_confirm(), after 3 presses.
bool shelly_cal_save(const cal_ratios_t *ratios);

#ifdef __cplusplus
}
#endif
