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
// docs/BUTTON-AND-LED.md is the source of truth. Its B-N-xx, B-M-xx and
// SW-xx rows are each covered by a host test tagged with the row ID, and
// scripts/check-ui-spec.py fails the tests if a row has no test. Change the
// doc, this table and the tests together (see CLAUDE.md).

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_INPUT_TAP,           // 1 or 2 taps
    UI_INPUT_PAIRING,       // 3 or more taps
    UI_INPUT_WIFI_MODE,     // Hold released at 5-10 s
    UI_INPUT_FACTORY_RESET, // Hold released at 10-20 s
    UI_INPUT_SWITCH,        // The wall switch (SW) changed
} ui_input_t;

// What the wall switch terminal is wired to on this variant.
typedef enum {
    UI_SWITCH_TOGGLE,       // Light, Outlet, Mini, 1PM: toggles the relay
    UI_SWITCH_CONTACT,      // Opener: door contact sensor
    UI_SWITCH_BINDING,      // Light Switch: drives bound lights
} ui_switch_role_t;

typedef struct {
    bool maintenance;           // Running the Wi-Fi maintenance boot
    bool started;               // Matter is up (normal mode only)
    bool pending_verify;        // Running image not yet confirmed (first 60 s after an update)
    bool ota_busy;              // A Matter OTA download is in progress
    bool maintenance_available; // This build has Wi-Fi maintenance mode
    bool code_change_pending;   // Maintenance: a setup-code change waits on the page for 3 presses
    ui_switch_role_t switch_role;
} ui_state_t;

typedef enum {
    UI_IGNORE,              // Nothing, no feedback
    UI_REFUSE,              // Refusal flash
    UI_TOGGLE,              // Toggle through Matter (the relay follows OnOff)
    UI_LOCAL_TOGGLE,        // Toggle the relay directly (maintenance, no Matter)
    UI_VARIANT,             // The variant's own handling (contact report, binding)
    UI_OPEN_PAIRING,        // Open or restart the pairing window
    UI_ENTER_MAINTENANCE,   // Restart into Wi-Fi maintenance mode
    UI_EXIT_MAINTENANCE,    // Restart into normal mode
    UI_FACTORY_RESET,       // Factory reset now, through Matter
    UI_DEFER_RESET,         // Record a reset, restart, reset once Matter is up
    UI_CONFIRM_CODE,        // Save the setup code waiting on the maintenance page
} ui_outcome_t;

ui_outcome_t ui_decide(ui_input_t input, const ui_state_t *st);

#ifdef __cplusplus
}
#endif
