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

// hw_snapshot.h
//
// The hardware snapshot's data and its text forms: what the board's pins,
// sensors and power-meter chip read during the last normal boot, kept in
// memory that survives the restart into Wi-Fi mode and shown there
// (/hw.json). Pure C, no ESP-IDF includes, so the host tests cover it.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HW_SNAP_MAGIC    0x53574853u // 'SHWS'
#define HW_SNAP_VERSION  1u
#define HW_SNAP_CHANNELS 2
#define HW_SNAP_REGS     40

// What the power meter's register reads got to.
typedef enum {
    HW_ADE_NOT_RUN = 0,     // Not read yet this boot
    HW_ADE_RUNNING,         // Reading
    HW_ADE_NO_ANSWER,       // Nothing answered at the chip's address
    HW_ADE_BUS_ERROR,       // The I2C bus could not be set up
    HW_ADE_READ_ERRORS,     // Stopped after reads failed in a row
    HW_ADE_DONE,            // Registers read; measurements every minute
    HW_ADE_SKIPPED_CRASH,   // (Bring-up builds) not run after a crash while probing
    HW_ADE_STATE_COUNT,
} hw_ade_state_t;

typedef struct {
    uint16_t addr;
    uint8_t width;          // Bits: 8, 16, 24 or 32
    uint8_t ok;             // 1 if the last read succeeded
    uint32_t value;
} hw_snap_reg_t;

// Temperatures are tenths of a degree C; HW_SNAP_NO_TEMP when unknown.
#define HW_SNAP_NO_TEMP INT16_MIN

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t boots;             // Normal boots since power-on
    uint32_t reset_reason;      // esp_reset_reason() of this boot
    uint32_t uptime_s;          // At the last sample
    uint32_t heap_free;
    uint32_t heap_min;
    uint32_t heap_big;
    uint32_t relay_cmd;         // Bit n: relay n+1 written on
    uint32_t relay_pin;         // Bit n: relay n+1's pad reads high
    uint32_t sw_edges[HW_SNAP_CHANNELS];
    uint8_t channels;
    uint8_t sw_raw;             // Bit n: SWn+1 pin high now
    uint8_t sw_stable;          // Bit n: SWn+1 settled level high
    uint8_t button_raw;         // Button pin level (1 = high, released on this board)
    int16_t ntc_raw;            // Average ADC count, -1 if none
    int16_t ntc_mv;             // Average millivolts, -1 if none
    int16_t ntc_c10;
    int16_t die_c10;
    uint8_t ntc_ok;
    uint8_t ntc_status;
    uint8_t governing_ntc;
    uint8_t overtemp;
    uint8_t no_sensor;
    uint8_t ade_state;          // hw_ade_state_t
    uint8_t ade_guard;          // (Bring-up builds) set while the probe talked to the chip
    uint8_t ade_count;          // Valid entries in ade[]
    int32_t ade_err;            // Last I2C error (esp_err_t), 0 if none
    uint32_t ade_dump_s;        // Uptime of the full dump
    uint32_t ade_live_s;        // Uptime of the last live read
    uint32_t ade_live_reads;    // Live read cycles done
    hw_snap_reg_t ade[HW_SNAP_REGS];
    uint32_t check;             // hw_snap_seal() over everything above
} hw_snap_t;

// What the Wi-Fi mode boot reads at the moment the page asks.
typedef struct {
    uint32_t uptime_s;
    uint32_t heap_free;
    uint32_t heap_min;
    uint32_t relay_cmd;
    uint32_t relay_pin;
    uint8_t channels;
    uint8_t sw_raw;
    uint8_t button_raw;
    int16_t ntc_raw;
    int16_t ntc_mv;
    int16_t ntc_c10;
    int16_t die_c10;
} hw_live_t;

// Start a fresh snapshot (magic, version, empty values).
void hw_snap_reset(hw_snap_t *s);
// Recompute the check after a change.
void hw_snap_seal(hw_snap_t *s);
// Magic, version, bounds and check all intact.
bool hw_snap_valid(const hw_snap_t *s);

// Store a register value read from the power meter: updates its entry, or
// adds one while there is room. ok false keeps the previous value.
void hw_snap_set_reg(hw_snap_t *s, uint16_t addr, uint8_t width, bool ok, uint32_t value);
// A stored register; false if absent or never read.
bool hw_snap_get_reg(const hw_snap_t *s, uint16_t addr, uint32_t *value);

const char *hw_ade_state_name(uint8_t state);

// A one-line summary of at most 100 bytes for the diagnostics log, like
// "rel 11/11 sw 01/01 e 3,4 btn 1 ntc 1820mV 41.2C die 45.1C heap 172k"
// (relays written/pins read, SW pins now/settled, SW edges).
// Returns the length written (always NUL-terminated).
size_t hw_snap_line(const hw_snap_t *s, char *buf, size_t len);

// The JSON for /hw.json: {"live":{...},"last_boot":{...}|null}. live may be
// NULL. Returns the length it needs (like snprintf); compare with len.
size_t hw_snap_json(const hw_snap_t *last_boot, const hw_live_t *live, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
