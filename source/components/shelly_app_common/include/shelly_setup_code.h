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

// shelly_setup_code.h
//
// The device's own Matter setup code (see setup_code.h and
// docs/COMMISSIONING.md). The device makes a random code the first time it
// needs one and saves it in NVS (namespace "shelly_code"), which a factory
// reset keeps. The owner can replace it with their own 11-digit code from
// the Wi-Fi maintenance page, confirmed with 3 presses of the button
// (docs/BUTTON-AND-LED.md, B-26).
//
// If the code can't be saved, the device pairs with the shared test code
// for that boot, so it is always pairable. A build without Wi-Fi mode
// always uses the test code: it has no page to show another one.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <setup_code.h>

#ifdef __cplusplus
extern "C" {
#endif

// Normal mode. Install the commissionable data provider: call before
// esp_matter::start(). shelly_app_started() then logs the code in use.
void shelly_setup_code_install(void);
void shelly_setup_code_log(void);

// Wi-Fi maintenance mode (Matter is off: these never call CHIP).

// The saved code, creating and saving one if there is none. False if it
// couldn't be saved: *out is then the test code. *created says the code
// was made just now.
bool shelly_setup_code_current(setup_code_t *out, bool *created);

// Stage a change: the owner's 11-digit code, or a new random one. It waits
// in shelly_settings (shelly_change_*) for 3 presses of the button, and
// applies from the next normal boot.
setup_code_err_t shelly_setup_code_stage_manual(const char *text);
void shelly_setup_code_stage_random(void);

// Save a code (what confirming a staged change does). False if it couldn't
// be saved, or in a build without Wi-Fi mode.
bool shelly_setup_code_save(const setup_code_t *code);

#ifdef __cplusplus
}
#endif
