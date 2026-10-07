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

// bl0942_codec.c

#include "bl0942_codec.h"

#include <math.h>

// MODE register bits (datasheet §3.2).
#define MODE_RESV               0x03
#define MODE_CF_EN              0x04
#define MODE_RMS_UPDATE_SEL     0x08    // 800ms RMS refresh
#define MODE_AC_FREQ_SEL        0x20    // 60 Hz
#define MODE_CF_CNT_ADD_SEL     0x80    // Accumulate absolute energy

// Readings at or above this fraction of the current full scale are
// treated as clipped.
#define CLIP_FRACTION           0.95f

static uint32_t u24(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

static int32_t s24(const uint8_t *p)
{
    uint32_t v = u24(p);
    if (v & 0x800000u) {
        v |= 0xFF000000u;
    }
    return (int32_t)v;
}

bl0942_frame_result_t bl0942_parse_frame(const uint8_t *buf, size_t len, bl0942_raw_t *out)
{
    if (len < BL0942_FRAME_LEN) {
        return BL0942_FRAME_SHORT;
    }
    if (buf[0] != BL0942_FRAME_HEADER) {
        return BL0942_FRAME_BAD_HEADER;
    }
    uint8_t sum = BL0942_CMD_READ;
    for (int i = 0; i < BL0942_FRAME_LEN - 1; i++) {
        sum += buf[i];
    }
    if ((uint8_t)~sum != buf[BL0942_FRAME_LEN - 1]) {
        return BL0942_FRAME_BAD_CHECKSUM;
    }

    out->i_rms = u24(&buf[1]);
    out->v_rms = u24(&buf[4]);
    out->i_fast_rms = u24(&buf[7]);
    out->watt = s24(&buf[10]);
    out->cf_cnt = u24(&buf[13]);
    out->freq = (uint16_t)(buf[16] | (buf[17] << 8));
    out->status = buf[19];
    return BL0942_FRAME_OK;
}

void bl0942_build_write(uint8_t reg, uint32_t value, uint8_t out[6])
{
    out[0] = BL0942_CMD_WRITE;
    out[1] = reg;
    out[2] = value & 0xFF;
    out[3] = (value >> 8) & 0xFF;
    out[4] = (value >> 16) & 0xFF;
    out[5] = (uint8_t)~(out[0] + out[1] + out[2] + out[3] + out[4]);
}

bool bl0942_parse_reg_response(uint8_t reg, const uint8_t resp[4], uint32_t *value)
{
    uint8_t sum = (uint8_t)(BL0942_CMD_READ + reg + resp[0] + resp[1] + resp[2]);
    if ((uint8_t)~sum != resp[3]) {
        return false;
    }
    *value = u24(resp);
    return true;
}

uint32_t bl0942_mode_value(bool line_60hz)
{
    uint32_t mode = MODE_RESV | MODE_CF_EN | MODE_CF_CNT_ADD_SEL | MODE_RMS_UPDATE_SEL;
    if (line_60hz) {
        mode |= MODE_AC_FREQ_SEL;
    }
    return mode;
}

void bl0942_cal_init(bl0942_cal_t *cal, float voltage_ref, float current_ref, float current_full_scale_a)
{
    cal->voltage_ref = voltage_ref;
    cal->current_ref = current_ref;
    // Datasheet gains: the RMS registers scale by 305978 and 73989 per
    // volt at the pins, WATT by 3537 per volt², and one CF pulse is
    // 419430.4 WATT-register seconds.
    cal->power_ref = (voltage_ref * current_ref * 3537.0f / 305978.0f) / 73989.0f;
    cal->pulses_per_kwh = cal->power_ref * 3600000.0f / 419430.4f;
    cal->current_full_scale_a = current_full_scale_a;
}

void bl0942_scale(const bl0942_cal_t *cal, const bl0942_raw_t *raw, bl0942_reading_t *out)
{
    out->volts = (float)raw->v_rms / cal->voltage_ref;
    out->amps = (float)raw->i_rms / cal->current_ref;
    out->watts = fabsf((float)raw->watt / cal->power_ref);
    out->hz = raw->freq != 0 ? 1000000.0f / (float)raw->freq : NAN;
    out->amps_clipped = out->amps >= cal->current_full_scale_a * CLIP_FRACTION;
}

double bl0942_mwh_per_pulse(const bl0942_cal_t *cal)
{
    return 1000000.0 / (double)cal->pulses_per_kwh;
}
