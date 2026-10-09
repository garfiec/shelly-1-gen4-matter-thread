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


// ade7953_codec.c

#include "ade7953_codec.h"

#include <math.h>
#include <stddef.h>

#define V_DIV       26000.0f
#define I_DIV       100000.0f
#define P_DIV       154.0f
#define PERIOD_HZ   223750.0f

bool ade7953_write_allowed(uint16_t addr, uint32_t value)
{
    return (addr == ADE7953_REG_UNLOCK && value == ADE7953_UNLOCK_VALUE) ||
           (addr == ADE7953_REG_SETUP && value == ADE7953_SETUP_VALUE);
}

float ade7953_volts_per_count(const ade7953_cal_t *cal)
{
    return cal->v_mul / V_DIV;
}

float ade7953_amps_per_count(const ade7953_cal_t *cal, int ch)
{
    return cal->i_mul[ch] / I_DIV;
}

float ade7953_watts_per_count(const ade7953_cal_t *cal, int ch)
{
    // The magnitude: the sign of the board's wiring is not a calibration.
    return fabsf(cal->p_mul[ch]) / P_DIV;
}

void ade7953_scale(const ade7953_cal_t *cal, const ade7953_raw_t *raw, ade7953_reading_t *out)
{
    out->volts = (float)raw->vrms * ade7953_volts_per_count(cal);
    // PERIOD counts a 223.75 kHz clock per line cycle; 0 and all ones are
    // what it reads without a line cycle.
    out->hz = raw->period == 0 || raw->period == 0xFFFF ? NAN : PERIOD_HZ / ((float)raw->period + 1.0f);
    for (int ch = 0; ch < ADE7953_CHANNELS; ch++) {
        out->amps[ch] = (float)raw->irms[ch] * ade7953_amps_per_count(cal, ch);
        out->watts[ch] = fabsf((float)raw->watt[ch]) * ade7953_watts_per_count(cal, ch);
    }
}

uint32_t ade7953_be_value(const uint8_t *bytes, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        v = (v << 8) | bytes[i];
    }
    return v;
}
