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

// shelly_fault_logic.h
//
// Pure fault-source bookkeeping, shared by the firmware and the host tests.
// No ESP-IDF or Matter dependencies.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Every source that can raise a hardware fault. Several sources share a
// Matter HardwareFaultEnum value, so the registry tracks sources and only
// publishes the de-duplicated set of enum values.
typedef enum {
    SHELLY_FAULT_SRC_OVERTEMP = 0,      // Board over temperature (thermal trip)
    SHELLY_FAULT_SRC_TEMP_SENSOR,       // No usable temperature sensor
    SHELLY_FAULT_SRC_NTC_OPEN,          // NTC open circuit (die sensor fallback)
    SHELLY_FAULT_SRC_OVERCURRENT,       // I²t overcurrent trip (latched)
    SHELLY_FAULT_SRC_WELDED_RELAY,      // Current flowing while commanded off
    SHELLY_FAULT_SRC_OVERVOLTAGE,       // Mains overvoltage trip
    SHELLY_FAULT_SRC_METER_LOST,        // No valid power-meter frame
    SHELLY_FAULT_SRC_COUNT
} shelly_fault_src_t;

// Matter GeneralDiagnostics HardwareFaultEnum values used here.
#define SHELLY_HW_FAULT_UNSPECIFIED          0
#define SHELLY_HW_FAULT_SENSOR               2
#define SHELLY_HW_FAULT_RESETTABLE_OVER_TEMP 3
#define SHELLY_HW_FAULT_POWER_SOURCE         5

static inline uint8_t shelly_fault_src_to_enum(shelly_fault_src_t src)
{
    switch (src) {
    case SHELLY_FAULT_SRC_OVERTEMP:
        return SHELLY_HW_FAULT_RESETTABLE_OVER_TEMP;
    case SHELLY_FAULT_SRC_TEMP_SENSOR:
    case SHELLY_FAULT_SRC_NTC_OPEN:
    case SHELLY_FAULT_SRC_METER_LOST:
        return SHELLY_HW_FAULT_SENSOR;
    case SHELLY_FAULT_SRC_OVERVOLTAGE:
        return SHELLY_HW_FAULT_POWER_SOURCE;
    case SHELLY_FAULT_SRC_OVERCURRENT:
    case SHELLY_FAULT_SRC_WELDED_RELAY:
    default:
        return SHELLY_HW_FAULT_UNSPECIFIED;
    }
}

// Convert a bitmask of active sources (bit n = source n) to the sorted,
// de-duplicated list of HardwareFaultEnum values. An enum value stays in
// the list while any of its sources is active. Returns the list length.
static inline size_t shelly_fault_mask_to_list(uint32_t mask, uint8_t *out, size_t max)
{
    uint32_t enums = 0;
    for (int src = 0; src < SHELLY_FAULT_SRC_COUNT; src++) {
        if (mask & (1u << src)) {
            enums |= 1u << shelly_fault_src_to_enum((shelly_fault_src_t)src);
        }
    }
    size_t n = 0;
    for (uint8_t e = 0; e < 32 && n < max; e++) {
        if (enums & (1u << e)) {
            out[n++] = e;
        }
    }
    return n;
}

#ifdef __cplusplus
}
#endif
