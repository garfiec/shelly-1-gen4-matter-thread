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


// metering_live.h
//
// The meter's live readings for the Wi-Fi maintenance page (/meter.json),
// and the averaged readings a calibration is computed from. No Matter
// dependencies. Without a meter (CONFIG_SHELLY_HAS_METER off) both report
// nothing.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sdkconfig.h>

#include <calibration_logic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define METERING_LIVE_CHANNELS 2

typedef struct {
    bool valid;                     // A reading arrived within the meter-lost time
    int channels;
    const char *chip;               // "BL0942" or "ADE7953"
    float volts;
    float hz;
    float amps[METERING_LIVE_CHANNELS];      // Calibrated; what Matter reports
    float watts[METERING_LIVE_CHANNELS];
    bool relay_on[METERING_LIVE_CHANNELS];
    int64_t energy_mwh[METERING_LIVE_CHANNELS];
    // The last raw register values (counts), for diagnosis.
    int64_t raw_v;
    int64_t raw_i[METERING_LIVE_CHANNELS];
    int64_t raw_p[METERING_LIVE_CHANNELS];
    cal_ratios_t ratios;            // The calibration in use
    bool steady_ready;              // The averaging window is full
} metering_live_t;

#if CONFIG_SHELLY_HAS_METER

bool metering_live(metering_live_t *out);

// The readings of relay ch averaged over the last 5 s, with the calibration
// in use applied, and how much each varied over that time ((max - min) /
// mean). NAN where the window is not full yet. False without a meter or
// for a relay the board doesn't have.
bool metering_cal_measure(int ch, cal_values_t *mean, cal_values_t *spread);

#else

static inline bool metering_live(metering_live_t *out)
{
    (void)out;
    return false;
}

static inline bool metering_cal_measure(int ch, cal_values_t *mean, cal_values_t *spread)
{
    (void)ch;
    (void)mean;
    (void)spread;
    return false;
}

#endif

#ifdef __cplusplus
}
#endif
