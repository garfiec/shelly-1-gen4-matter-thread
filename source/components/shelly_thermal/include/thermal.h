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

// thermal.h

#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the thermal monitor.
//
// Configures the ESP32-C6 internal temperature sensor and, on boards with
// one (CONFIG_SHELLY_HAS_NTC), the board NTC. Registers a relay gate and
// spawns a background task that polls every 2 seconds.
//
// The governing sensor is the NTC when fitted and plausible, otherwise the
// die sensor. If the governing temperature stays above its trip threshold
// for CONFIG_SHELLY_THERMAL_SUSTAIN_S seconds, the monitor:
//   1. Trips the relay (relay_trip())
//   2. Updates the Matter OnOff attribute to Off
//   3. Raises the over-temperature hardware fault and refuses turn-on
//      until the temperature drops below the clear threshold.
//
// If no sensor gives a usable reading (install failure, or three failed
// reads in a row), a Sensor hardware fault is raised and turn-on is
// refused until a reading recovers. An NTC that reads open (or cannot be
// read) three times in a row falls back to the die sensor and raises a
// Sensor fault.
//
// The first check runs before this returns. Returns ESP_OK on success.
esp_err_t thermal_init(void);

// Set the endpoints the monitor reports to: the OnOff endpoint it turns
// off on a trip, and the Temperature Sensor endpoint that publishes the
// governing temperature. Call after the endpoints are created.
void thermal_set_endpoints(uint16_t relay_endpoint_id, uint16_t temperature_endpoint_id);

#ifdef __cplusplus
}
#endif
