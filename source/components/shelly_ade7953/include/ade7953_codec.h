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


// ade7953_codec.h
//
// The ADE7953 power meter's scaling, and the only writes the firmware may
// send it. Pure C with no ESP-IDF dependencies, so the host tests cover it.
//
// Scaling follows ESPHome's ade7953 component, so calibration measured with
// ESPHome (automatous-io/shelly-gen4-esphome) carries over as multipliers:
//   V  = VRMS  / 26000  * v_mul
//   I  = IRMS  / 100000 * i_mul
//   P  = WATT  / 154    * p_mul       (signed register; p_mul may be negative)
//   Hz = 223750 / (PERIOD + 1)
// The chip's own gain registers stay at their reset values: all calibration
// is in these multipliers, so a power cycle always leaves the chip in a
// known state.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADE7953_CHANNELS        2

// The setup the datasheet asks for after every reset: unlock 0x120, then
// write it. Nothing else is ever written.
#define ADE7953_REG_UNLOCK      0x0FE
#define ADE7953_UNLOCK_VALUE    0xADu
#define ADE7953_REG_SETUP       0x120
#define ADE7953_SETUP_VALUE     0x0030u

// The registers read on every poll.
#define ADE7953_REG_VRMS        0x31C
#define ADE7953_REG_IRMSA       0x31A
#define ADE7953_REG_IRMSB       0x31B
#define ADE7953_REG_AWATT       0x312
#define ADE7953_REG_BWATT       0x313
#define ADE7953_REG_PERIOD      0x10E

// True only for the two setup writes above, with their values.
bool ade7953_write_allowed(uint16_t addr, uint32_t value);

typedef struct {
    uint32_t vrms;
    uint32_t irms[ADE7953_CHANNELS];
    int32_t watt[ADE7953_CHANNELS];
    uint16_t period;
} ade7953_raw_t;

typedef struct {
    float v_mul;
    float i_mul[ADE7953_CHANNELS];
    float p_mul[ADE7953_CHANNELS];
} ade7953_cal_t;

typedef struct {
    float volts;
    float hz;                           // NAN when the chip reports no period
    float amps[ADE7953_CHANNELS];
    float watts[ADE7953_CHANNELS];      // Absolute: reverse power counts as import
} ade7953_reading_t;

// Units per register count, for the shared calibration (calibration_logic.h).
float ade7953_volts_per_count(const ade7953_cal_t *cal);
float ade7953_amps_per_count(const ade7953_cal_t *cal, int ch);
float ade7953_watts_per_count(const ade7953_cal_t *cal, int ch);

void ade7953_scale(const ade7953_cal_t *cal, const ade7953_raw_t *raw, ade7953_reading_t *out);

// Join a register's big-endian bytes (as read over I2C) into its value.
uint32_t ade7953_be_value(const uint8_t *bytes, int n);

#ifdef __cplusplus
}
#endif
