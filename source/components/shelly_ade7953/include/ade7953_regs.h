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


// ade7953_regs.h
//
// The ADE7953 power-meter registers the read-only probe reads, and the ones
// it must never read. Pure C, so the host tests check the table.
//
// Addresses and widths follow the ADE7953 datasheet's register map (the
// register width is set by the address range) and match the registers
// ESPHome's and Tasmota's ADE7953 drivers read on Shelly boards.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADE7953_I2C_ADDR 0x38

typedef struct {
    uint16_t addr;
    const char *name;
    bool live;      // Also read every minute after the dump
} ade7953_reg_t;

extern const ade7953_reg_t ADE7953_PROBE_REGS[];
extern const int ADE7953_PROBE_REG_COUNT;

// Register width in bits from the address: 0x000-0x0FF 8, 0x100-0x1FF 16,
// 0x200-0x2FF 24, 0x300-0x3FF 32; 0x702 (VERSION) and 0x800 (EX_REF) 8.
// 0 for an address outside the map.
int ade7953_reg_width(uint16_t addr);

// Registers whose read changes the chip: the energy accumulators (cleared on
// read by default), the peak and interrupt-status reset-on-read registers,
// in both their 24-bit and 32-bit addresses. The probe never reads these.
bool ade7953_reg_read_resets(uint16_t addr);

#ifdef __cplusplus
}
#endif
