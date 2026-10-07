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

// button_logic.h
//
// The button gesture recognizer: turns press and release edges into taps,
// multi-presses and timed holds. Pure C, no ESP-IDF includes, so the host
// tests can drive it.
//
// This is part of the device UI. docs/BUTTON-AND-LED.md is the source of
// truth for every gesture and timing; the T-xx IDs below are quoted there
// and checked by scripts/check-ui-spec.py. Change both together (see
// CLAUDE.md).
//
// Gestures, all acted on at release, never while held:
//   tap (released before BUTTON_TAP_MAX_MS), then no press for
//   BUTTON_MULTI_GAP_MS          -> TOGGLE (1 or 2 taps) or PAIRING (3+)
//   hold released at 5-10 s      -> WIFI_MODE
//   hold released at 10-20 s     -> FACTORY_RESET
//   hold released at 1-5 s, 20 s+ -> nothing (a 20 s+ hold cancels)
// A hold during a tap sequence discards the taps.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BUTTON_TAP_MAX_MS           1000    // T-01
#define BUTTON_MULTI_GAP_MS         300     // T-02
#define BUTTON_HOLD_WIFI_MS         5000    // T-03
#define BUTTON_HOLD_RESET_MS        10000   // T-04
#define BUTTON_HOLD_CANCEL_MS       20000   // T-05
#define BUTTON_PAIRING_TAPS         3       // T-06

typedef enum {
    BUTTON_ACTION_NONE,
    BUTTON_ACTION_TOGGLE,
    BUTTON_ACTION_PAIRING,
    BUTTON_ACTION_WIFI_MODE,
    BUTTON_ACTION_FACTORY_RESET,
} button_action_t;

// What releasing now would do, for the LED while the button is held.
typedef enum {
    BUTTON_HINT_NONE,       // Not held, or a tap so far
    BUTTON_HINT_WAIT,       // 1-5 s: nothing yet
    BUTTON_HINT_WIFI,       // 5-10 s: Wi-Fi mode
    BUTTON_HINT_RESET,      // 10-20 s: factory reset
} button_hint_t;

typedef struct {
    bool pressed;
    uint32_t press_ms;
    uint32_t release_ms;
    uint8_t taps;           // Taps waiting for the multi-press gap to end
} button_logic_t;

void button_logic_init(button_logic_t *b);

// Feed the current level at now_ms (any monotonic millisecond clock; it may
// wrap). Call on every edge, and every few tens of ms while
// button_logic_busy() is true, so pending taps are resolved on time.
// Returns the action a gesture completed, if any.
button_action_t button_logic_update(button_logic_t *b, bool pressed, uint32_t now_ms);

// What releasing at now_ms would do.
button_hint_t button_logic_hint(const button_logic_t *b, uint32_t now_ms);

// True while held or while taps are pending.
bool button_logic_busy(const button_logic_t *b);

#ifdef __cplusplus
}
#endif
