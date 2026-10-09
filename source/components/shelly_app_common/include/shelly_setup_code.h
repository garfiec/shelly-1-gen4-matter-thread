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
// docs/COMMISSIONING.md). First match wins:
//   1. The owner's own code, entered on the Wi-Fi maintenance page and
//      confirmed with 3 presses (docs/BUTTON-AND-LED.md, B-26). Saved in NVS
//      (namespace "shelly_code"); a factory reset forgets it.
//   2. Shelly's sticker code, read (never written) from the stock "shelly"
//      partition each boot. Typing the sticker's code on the page goes back
//      to it.
//   3. A random code the device made the first time it needed one, saved
//      in NVS. Kept through factory resets.
//   4. The shared test code: when a generated code can't be saved, or in a
//      build without Wi-Fi mode and without a sticker code.

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

// The sticker's code, if the Shelly partition has a usable one.
bool shelly_setup_code_sticker(setup_code_t *out);

// The QR text for a code: the sticker's own exactly as printed for the
// sticker code, otherwise one with this firmware's vendor and product ID.
bool shelly_setup_code_qr(const setup_code_t *code, char out[SETUP_CODE_QR_MAX]);

// Stage a change: the owner's 11-digit code, or a new random one. The
// sticker's own code, typed, stages the sticker code instead. It waits
// in shelly_settings (shelly_change_*) for 3 presses of the button, and
// applies from the next normal boot.
setup_code_err_t shelly_setup_code_stage_manual(const char *text);
void shelly_setup_code_stage_random(void);

// Save a code (what confirming a staged change does). For the sticker code,
// forgets the owner's own code instead. False if it couldn't be saved, or
// in a build without Wi-Fi mode.
bool shelly_setup_code_save(const setup_code_t *code);

// A factory reset: forget the owner's own code, so the device goes back to
// its sticker code (or its generated one). Normal mode, before
// esp_matter::factory_reset().
void shelly_setup_code_factory_reset(void);

#ifdef __cplusplus
}
#endif
