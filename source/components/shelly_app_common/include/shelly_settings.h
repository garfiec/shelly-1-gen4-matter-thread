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

// shelly_settings.h
//
// The device settings changed on the Wi-Fi maintenance page: the Matter
// setup code (shelly_setup_code.h) and the wall-switch mode. A change made
// on the page waits here until the button is pressed 3 times within
// SHELLY_CHANGE_CONFIRM_S (docs/BUTTON-AND-LED.md, B-26), so nobody on the
// open network can change a setting without the device in hand.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <setup_code.h>
#include <ui_policy.h>

#ifdef __cplusplus
extern "C" {
#endif

// Load the saved settings. shelly_ui_init() calls it, after NVS is up.
void shelly_settings_init(void);

// The wall-switch mode (toggle role only). Toggle if none is saved or the
// saved value is unknown.
ui_switch_mode_t shelly_switch_mode_get(void);
// The same for one input (0 = SW1). Each input of a 2-channel board has its own.
ui_switch_mode_t shelly_switch_mode_get_ch(int ch);

// ---- The change waiting for 3 presses ----

#define SHELLY_CHANGE_CONFIRM_S 120

typedef enum {
    SHELLY_CHANGE_NONE = 0,
    SHELLY_CHANGE_SETUP_CODE,
    SHELLY_CHANGE_SWITCH_MODE,
} shelly_change_kind_t;

typedef struct {
    shelly_change_kind_t kind;
    setup_code_t code;              // SHELLY_CHANGE_SETUP_CODE
    ui_switch_mode_t switch_mode;   // SHELLY_CHANGE_SWITCH_MODE
    uint8_t channel;                // SHELLY_CHANGE_SWITCH_MODE: which input (0 = SW1)
} shelly_change_t;

// Stage a change. It replaces any change already waiting.
void shelly_change_stage(const shelly_change_t *change);

// Stage a switch-mode change. False if this variant's wall switch has no
// modes (the Opener, the Light Switch) or the mode is not valid.
bool shelly_switch_mode_stage(ui_switch_mode_t mode);
bool shelly_switch_mode_stage_ch(int ch, ui_switch_mode_t mode);

void shelly_change_cancel(void);

// The waiting change and the seconds left to confirm it; false if none.
bool shelly_change_pending(shelly_change_t *out, uint32_t *secs_left);

// Save the waiting change: a setup code applies from the next normal boot,
// a switch mode at once. False if none is waiting or the save failed.
bool shelly_change_confirm(void);

#ifdef __cplusplus
}
#endif
