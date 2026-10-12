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

// contact_sensor.h

#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

// Door-position contact sensor on the Shelly 1 Gen4 SW terminal (GPIO10).
//
// In the opener variant the SW terminal is a reed/limit switch reporting
// door state, NOT a wall toggle. Each debounced change of level (see the
// shared switch_input module) updates the Matter Boolean State cluster's
// StateValue on the contact sensor endpoint.
//
// StateValue semantics (Matter): true = CLOSED (contact present),
// false = OPEN. The level->state mapping is set by CONTACT_OPEN_IS_HIGH
// in app_priv.h (default suits a normally open reed).
//
// HARDWARE NOTE: The Shelly 1 Gen4 PCB has an external pull resistor on
// GPIO10. Do not enable the internal pullup or pulldown — internal pulls
// fight the external circuit and prevent the ISR from firing. See
// switch_input.cpp for details.

// Initialize the contact sensor input. Configures GPIO10 as an
// interrupt-driven input and starts the debounce task. Must be called
// before contact_sensor_read_state(). Returns ESP_OK on success.
esp_err_t contact_sensor_init(void);
// The Contact Sensor endpoint, once created.
void contact_sensor_set_endpoint(uint16_t endpoint_id);

// Read the current contact state as the Matter StateValue
// (true = closed, false = open), applying CONTACT_OPEN_IS_HIGH. Used to
// seed the endpoint's initial value at boot.
bool contact_sensor_read_state(void);

// Push the current contact state to the Boolean State cluster. Called by
// the device UI for each debounced change once Matter is up (the
// switch_handler), and once after Matter starts to publish the boot state.
// Takes the CHIP stack lock internally.
void contact_sensor_report(void);
