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

// ui_policy.h
//
// What each user input does in each device state: the device UI's
// decision table. Pure C, no ESP-IDF includes, so the host tests walk it
// row by row.
//
// docs/BUTTON-AND-LED.md is the source of truth. Its B-xx, SW-xx and SM-xx
// rows are each covered by a host test tagged with the row ID, and
// scripts/check-ui-spec.py fails the tests if a row has no test. Change the
// doc, this table and the tests together (see CLAUDE.md).

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_INPUT_TAP,           // 1 tap
    UI_INPUT_PAIRING,       // 3 or more taps
    UI_INPUT_WIFI_MODE,     // Hold released at 5-10 s
    UI_INPUT_FACTORY_RESET, // Hold released at 10-20 s
    UI_INPUT_SWITCH,        // The wall switch (SW) changed
    UI_INPUT_TAP2,          // Exactly 2 taps: decided as a tap, on relay 2 of a 2-channel board
} ui_input_t;

// What the wall switch terminal is wired to on this variant.
typedef enum {
    UI_SWITCH_TOGGLE,       // Light, Outlet, Mini, 1PM: toggles the relay
    UI_SWITCH_CONTACT,      // Opener: door contact sensor
    UI_SWITCH_BINDING,      // Light Switch: drives bound lights
} ui_switch_role_t;

// What a toggle-role wall switch does (set on the Wi-Fi mode page).
typedef enum {
    UI_SWITCH_MODE_TOGGLE = 0,  // Every change toggles (the default)
    UI_SWITCH_MODE_PUSH,        // A push button: toggles when pressed
    UI_SWITCH_MODE_FOLLOW,      // The relay follows the switch position
    UI_SWITCH_MODE_DISABLED,    // The switch does nothing
    UI_SWITCH_MODE_COUNT,
} ui_switch_mode_t;

typedef struct {
    bool maintenance;           // Running the Wi-Fi maintenance boot
    bool started;               // Matter is up (normal mode only)
    bool pending_verify;        // Running image not yet confirmed (first 60 s after an update)
    bool ota_busy;              // A Matter OTA download is in progress
    bool maintenance_available; // This build has Wi-Fi maintenance mode
    bool change_pending;        // Maintenance: a change on the page waits for 3 presses
    ui_switch_role_t switch_role;
    ui_switch_mode_t switch_mode; // Toggle role only
    bool switch_active;         // UI_INPUT_SWITCH: the switch is now on (pressed)
    uint8_t channels;           // Relays on the board; 0 means 1
    uint8_t switch_channel;     // UI_INPUT_SWITCH: which input changed (0 = SW1)
} ui_state_t;

typedef enum {
    UI_IGNORE,              // Nothing, no feedback
    UI_REFUSE,              // Refusal flash
    UI_TOGGLE,              // Toggle through Matter (the relay follows OnOff)
    UI_LOCAL_TOGGLE,        // Toggle the relay directly (maintenance, no Matter)
    UI_TURN_ON,             // Turn on through Matter (a no-op if already on)
    UI_TURN_OFF,            // Turn off through Matter (a no-op if already off)
    UI_LOCAL_TURN_ON,       // Turn the relay on directly (maintenance)
    UI_LOCAL_TURN_OFF,      // Turn the relay off directly (maintenance)
    UI_VARIANT,             // The variant's own handling (contact report, binding)
    UI_OPEN_PAIRING,        // Open or restart the pairing window
    UI_ENTER_MAINTENANCE,   // Restart into Wi-Fi maintenance mode
    UI_EXIT_MAINTENANCE,    // Restart into normal mode
    UI_FACTORY_RESET,       // Factory reset now, through Matter
    UI_DEFER_RESET,         // Record a reset, restart, reset once Matter is up
    UI_CONFIRM_CHANGE,      // Save the change waiting on the maintenance page
} ui_outcome_t;

ui_outcome_t ui_decide(ui_input_t input, const ui_state_t *st);

// Which relay a relay outcome of ui_decide() acts on (0 = relay 1): a tap is
// relay 1, 2 taps relay 2 on a 2-channel board (relay 1 otherwise), and a
// wall switch its own relay.
uint8_t ui_decide_channel(ui_input_t input, const ui_state_t *st);

// The switch mode's name on the Wi-Fi page: "toggle", "push", "follow",
// "disabled". parse() is false for anything else.
const char *ui_switch_mode_name(ui_switch_mode_t mode);
bool ui_switch_mode_parse(const char *name, ui_switch_mode_t *out);

#ifdef __cplusplus
}
#endif
