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

// status_led.h

#pragma once

#include <esp_err.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// The device UI (LED patterns, their priority and meaning) is documented in
// docs/BUTTON-AND-LED.md. Change that doc with any change here (CLAUDE.md).
// The patterns themselves are data, in led_patterns.h.

typedef enum {
    LED_STATE_OFF,                  // Booting
    LED_STATE_BLE_ADVERTISING,      // Pairing window open (paired or not)
    LED_STATE_UNPAIRED_IDLE,        // Not paired, pairing window closed
    LED_STATE_THREAD_CONNECTING,    // Paired, joining Thread
    LED_STATE_THREAD_CONNECTED,     // Solid on: fully operational
    LED_STATE_MAINTENANCE,          // Wi-Fi maintenance mode
    LED_STATE_UPLOADING,            // Maintenance mode, firmware upload running
} led_state_t;

// What the button is about to do if released now. Shown above everything
// else while the button is held.
typedef enum {
    LED_HINT_NONE,
    LED_HINT_HOLD_WAIT,             // Held 1-5 s: nothing yet, keep holding
    LED_HINT_HOLD_WIFI,             // Release = Wi-Fi mode (exit, in maintenance)
    LED_HINT_HOLD_RESET,            // Release = factory reset
} led_hint_t;

// Initialize the status LED system (CONFIG_SHELLY_LED_GPIO). Call once
// during boot.
esp_err_t status_led_init(void);

// Change the base LED state. Safe to call from any task.
// Priority, highest first: hold hint > one-shot flash > Identify > fault >
// base, except that the maintenance states (MAINTENANCE, UPLOADING) show
// over a fault.
void status_led_set_state(led_state_t state);

// Enter Identify mode.
void status_led_start_identify(void);

// Exit Identify mode. Restores the fault or base state.
void status_led_stop_identify(void);

// Show (true) or clear (false) the fault pattern. A fault overrides the
// base state (apart from Wi-Fi maintenance mode) but not Identify.
void status_led_set_fault(bool active);

// Show the button hold hint, or LED_HINT_NONE to clear it.
void status_led_set_hint(led_hint_t hint);

// One-shot feedback, played once over Identify, fault and the base state.
// A new one-shot replaces one still playing.
void status_led_flash_press(void);      // A button press was registered
void status_led_flash_refused(void);    // The requested action was refused
void status_led_flash_accepted(void);   // The requested action was accepted

#ifdef __cplusplus
}
#endif
