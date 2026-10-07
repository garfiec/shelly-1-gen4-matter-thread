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

// bl0942_codec.h
//
// BL0942 energy-meter UART protocol: frame encode/decode and scaling.
// Pure C with no ESP-IDF dependencies, so the host tests can run it.
//
// Protocol from the BL0942 datasheet (V1.05). The chip answers a
// read-all request (0x58 0xAA) with a 23-byte frame:
//
//   0      header 0x55
//   1..3   I_RMS       u24 LE
//   4..6   V_RMS       u24 LE
//   7..9   I_FAST_RMS  u24 LE
//   10..12 WATT        s24 LE
//   13..15 CF_CNT      u24 LE
//   16..17 FREQ        u16 LE (period in µs; reset value 0x4E20 = 50.00 Hz)
//   18     reserved
//   19     STATUS
//   20..21 reserved
//   22     checksum = ~(0x58 + sum of bytes 0..21)

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BL0942_FRAME_LEN        23
#define BL0942_FRAME_HEADER     0x55
#define BL0942_CMD_READ         0x58
#define BL0942_CMD_WRITE        0xA8
#define BL0942_FULL_PACKET      0xAA

#define BL0942_REG_MODE         0x19
#define BL0942_REG_SOFT_RESET   0x1C
#define BL0942_REG_USR_WRPROT   0x1D

#define BL0942_USR_WRPROT_UNLOCK    0x55
#define BL0942_FREQ_RESET_VALUE     0x4E20

typedef struct {
    uint32_t i_rms;
    uint32_t v_rms;
    uint32_t i_fast_rms;
    int32_t watt;
    uint32_t cf_cnt;
    uint16_t freq;
    uint8_t status;
} bl0942_raw_t;

typedef enum {
    BL0942_FRAME_OK = 0,
    BL0942_FRAME_SHORT,
    BL0942_FRAME_BAD_HEADER,
    BL0942_FRAME_BAD_CHECKSUM,
} bl0942_frame_result_t;

// Parse a read-all response. buf must hold at least BL0942_FRAME_LEN bytes.
bl0942_frame_result_t bl0942_parse_frame(const uint8_t *buf, size_t len, bl0942_raw_t *out);

// Build a register write (6 bytes).
void bl0942_build_write(uint8_t reg, uint32_t value, uint8_t out[6]);

// Validate a single-register read response (3 data bytes + checksum) and
// extract the value. Returns false on a checksum mismatch.
bool bl0942_parse_reg_response(uint8_t reg, const uint8_t resp[4], uint32_t *value);

// The MODE register value this firmware uses: CF output enabled, CF_CNT
// accumulates absolute energy (reverse power counts as import), RMS
// updated every 800ms, line frequency 50 or 60 Hz.
uint32_t bl0942_mode_value(bool line_60hz);

// Calibration. voltage_ref and current_ref are counts per volt and per
// amp, measured on the board. Power and energy references are derived
// from them with the same formulas ESPHome uses, so references measured
// with ESPHome (automatous-io/shelly-gen4-esphome) apply unchanged.
typedef struct {
    float voltage_ref;      // counts per V
    float current_ref;      // counts per A
    float power_ref;        // counts per W
    float pulses_per_kwh;   // CF_CNT pulses per kWh
    float current_full_scale_a; // Current channel clip level (A rms)
} bl0942_cal_t;

void bl0942_cal_init(bl0942_cal_t *cal, float voltage_ref, float current_ref, float current_full_scale_a);

typedef struct {
    float volts;
    float amps;
    float watts;        // Absolute active power (reverse power counts as import)
    float hz;           // NAN if the chip reports no period
    bool amps_clipped;  // I_RMS at or near the current channel full scale
} bl0942_reading_t;

void bl0942_scale(const bl0942_cal_t *cal, const bl0942_raw_t *raw, bl0942_reading_t *out);

// Energy represented by one CF pulse, in mWh.
double bl0942_mwh_per_pulse(const bl0942_cal_t *cal);

#ifdef __cplusplus
}
#endif
