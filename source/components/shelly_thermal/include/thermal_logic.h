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

// thermal_logic.h
//
// Pure thermal logic, shared by the firmware and the host tests. No
// ESP-IDF or Matter dependencies.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Over-temperature trip with hysteresis. Trips once the temperature has
// stayed above trip_c for sustain_ms; clears as soon as it drops below
// clear_c. Between the two, the state holds.
typedef struct {
    float trip_c;
    float clear_c;
    int64_t sustain_ms;
    int64_t over_since_ms;  // -1 while not above trip_c
    bool tripped;
} thermal_hyst_t;

void thermal_hyst_init(thermal_hyst_t *h, float trip_c, float clear_c, int64_t sustain_ms);

// Feed one reading. Returns true if the tripped state changed.
bool thermal_hyst_update(thermal_hyst_t *h, float temp_c, int64_t now_ms);

typedef enum {
    NTC_READING_OK,
    NTC_READING_OPEN,   // At or near the high rail: open circuit
} ntc_status_t;

// NTC in the GND leg of a divider (ESPHome "DOWNSTREAM"): fixed resistor
// from the supply to the pin, NTC from the pin to GND. Hot means low
// resistance, so low voltage.
//
// A reading at or near the high rail is an open circuit (or colder than
// about -33°C, which a wall box never is) and is reported as OPEN. A
// reading at or near the low rail is NOT a fault: it converts to a very
// high temperature, so a shorted NTC trips on over-temperature.
typedef struct {
    float r_fixed_ohm;      // Divider resistor, supply side
    float r_nominal_ohm;    // NTC resistance at t_nominal_c
    float t_nominal_c;
    float beta;
    float vref_mv;          // Divider supply
} ntc_params_t;

ntc_status_t ntc_temperature_c(const ntc_params_t *p, float v_mv, float *temp_c);

#ifdef __cplusplus
}
#endif
