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


// ade7953_regs.c

#include "ade7953_regs.h"

#include <stddef.h>

const ade7953_reg_t ADE7953_PROBE_REGS[] = {
    // Identity and configuration
    {0x702, "VERSION", false},
    {0x102, "CONFIG", false},
    {0x001, "DISNOLOAD", false},
    {0x004, "LCYCMODE", false},
    {0x007, "PGA_V", false},
    {0x008, "PGA_IA", false},
    {0x009, "PGA_IB", false},
    {0x107, "CFMODE", false},
    {0x120, "RESERVED_120", false}, // 0x0030 once the datasheet's setup write was done
    {0x800, "EX_REF", false},
    // Calibration gains (left at their defaults by the chip after reset)
    {0x380, "AIGAIN", false},
    {0x381, "AVGAIN", false},
    {0x382, "AWGAIN", false},
    {0x383, "AVARGAIN", false},
    {0x384, "AVAGAIN", false},
    {0x38C, "BIGAIN", false},
    {0x38D, "BVGAIN", false},
    {0x38E, "BWGAIN", false},
    {0x38F, "BVARGAIN", false},
    {0x390, "BVAGAIN", false},
    // Interrupt enable and status (status is read without its reset)
    {0x32C, "IRQENA", false},
    {0x32D, "IRQSTATA", true},
    {0x32F, "IRQENB", false},
    {0x330, "IRQSTATB", false},
    // Measurements
    {0x10E, "PERIOD", true},
    {0x31C, "VRMS", true},
    {0x31A, "IRMSA", true},
    {0x31B, "IRMSB", true},
    {0x312, "AWATT", true},
    {0x313, "BWATT", true},
    {0x310, "AVA", true},
    {0x311, "BVA", true},
    {0x314, "AVAR", true},
    {0x315, "BVAR", true},
    {0x10A, "PFA", true},
    {0x10B, "PFB", true},
    {0x10C, "ANGLE_A", false},
    {0x10D, "ANGLE_B", false},
};

const int ADE7953_PROBE_REG_COUNT = (int)(sizeof(ADE7953_PROBE_REGS) / sizeof(ADE7953_PROBE_REGS[0]));

int ade7953_reg_width(uint16_t addr)
{
    if (addr == 0x702 || addr == 0x800) {
        return 8;
    }
    if (addr <= 0x0FF) {
        return 8;
    }
    if (addr <= 0x1FF) {
        return 16;
    }
    if (addr <= 0x2FF) {
        return 24;
    }
    if (addr <= 0x3FF) {
        return 32;
    }
    return 0;
}

bool ade7953_reg_read_resets(uint16_t addr)
{
    // The same register at its 24-bit (0x2xx) and 32-bit (0x3xx) address.
    uint16_t low = addr & 0x0FF;
    if (addr < 0x200 || addr > 0x3FF) {
        return false;
    }
    // AENERGYA, AENERGYB, RENERGYA, RENERGYB, APENERGYA, APENERGYB
    if (low >= 0x1E && low <= 0x23) {
        return true;
    }
    // RSTVPEAK, RSTIAPEAK, RSTIBPEAK, RSTIRQSTATA, RSTIRQSTATB
    return low == 0x27 || low == 0x29 || low == 0x2B || low == 0x2E || low == 0x31;
}
