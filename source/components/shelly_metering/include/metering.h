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

// metering.h
//
// Power metering for boards with a meter (CONFIG_SHELLY_HAS_METER): the
// BL0942 of the Shelly 1PM Gen4 or the two-channel ADE7953 of the Shelly
// 2PM Gen4.
//
// One task polls the meter every 200ms. Every sample feeds the electrical
// protection, whether or not metering is advertised to Matter. Metering is
// advertised as the Matter Electrical Sensor device type on each relay
// endpoint (Electrical Power Measurement, AC; Electrical Energy
// Measurement, imported cumulative energy) only once the board has seen AC
// mains: a board powered from DC never advertises a meter that would read
// zero. The calibration entered on the Wi-Fi maintenance page
// (shelly_calibration.h) applies on top of the compiled one.

#pragma once

#include <esp_err.h>
#include <esp_matter.h>

// Bring up the meter, load the calibration, detect AC mains (up to 3s),
// restore the energy totals, and start the poll task that feeds protection. Call after
// protection_init(), before creating the node.
esp_err_t metering_init(void);

// Add the Electrical Sensor device type and the metering clusters to the
// relay endpoint. Refuses (and logs) instead of creating a composition
// that esp-matter would abort on. Call before esp_matter::start().
// metering_add_to_endpoint() is relay 1's endpoint; the _ch form any relay's.
esp_err_t metering_add_to_endpoint(esp_matter::endpoint_t *endpoint);
esp_err_t metering_add_to_endpoint_ch(int ch, esp_matter::endpoint_t *endpoint);

// Call after esp_matter::start(): sets the energy accuracy, publishes the
// restored energy totals, and starts Matter reporting.
void metering_matter_started(void);
