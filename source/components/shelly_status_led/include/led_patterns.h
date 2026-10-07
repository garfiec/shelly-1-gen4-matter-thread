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

// led_patterns.h
//
// Every status LED pattern, as data. Pure C, no ESP-IDF includes, so the
// host tests can check it.
//
// This table is part of the device UI. docs/BUTTON-AND-LED.md is the source
// of truth and quotes each row by its LED-xx ID; scripts/check-ui-spec.py
// fails the host tests if the two disagree. Change both together (see
// CLAUDE.md).

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LED_PATTERN_MAX_STEPS 6

// A pattern is a run of steps. Step 0 has level first_on, and each later
// step flips the level. A pattern with no steps holds first_on forever.
// A repeating pattern loops; a one-shot pattern ends after its last step.
typedef struct {
    bool first_on;
    bool repeat;
    uint8_t steps;
    uint16_t ms[LED_PATTERN_MAX_STEPS];
} led_pattern_t;

typedef enum {
    LED_PAT_OFF,            // Booting
    LED_PAT_PAIRING,        // Pairing window open
    LED_PAT_UNPAIRED_IDLE,  // Not paired, pairing window closed
    LED_PAT_CONNECTING,     // Joining Thread
    LED_PAT_CONNECTED,      // Connected
    LED_PAT_IDENTIFY,       // Identify
    LED_PAT_FAULT,          // Hardware fault
    LED_PAT_MAINTENANCE,    // Wi-Fi maintenance mode
    LED_PAT_UPLOADING,      // Maintenance mode, firmware upload running
    LED_PAT_PRESS,          // One-shot: button press registered
    LED_PAT_HOLD_WAIT,      // Held 1-5 s: keep holding
    LED_PAT_HOLD_WIFI,      // Held 5-10 s: release = Wi-Fi mode (or exit it)
    LED_PAT_HOLD_RESET,     // Held 10-20 s: release = factory reset
    LED_PAT_REFUSED,        // One-shot: action refused
    LED_PAT_ACCEPTED,       // One-shot: action accepted
    LED_PAT_COUNT,
} led_pattern_id_t;

// Keep one row per line, tagged with its doc ID: check-ui-spec.py reads them.
static const led_pattern_t LED_PATTERNS[LED_PAT_COUNT] = {
    [LED_PAT_OFF]           = {false, true,  0, {0}},                          // LED-01
    [LED_PAT_PAIRING]       = {true,  true,  2, {200, 200}},                   // LED-02
    [LED_PAT_UNPAIRED_IDLE] = {true,  true,  2, {100, 1900}},                  // LED-03
    [LED_PAT_CONNECTING]    = {true,  true,  2, {500, 1500}},                  // LED-04
    [LED_PAT_CONNECTED]     = {true,  true,  0, {0}},                          // LED-05
    [LED_PAT_IDENTIFY]      = {true,  true,  2, {100, 900}},                   // LED-06
    [LED_PAT_FAULT]         = {true,  true,  2, {100, 100}},                   // LED-07
    [LED_PAT_MAINTENANCE]   = {true,  true,  4, {100, 100, 100, 700}},         // LED-08
    [LED_PAT_UPLOADING]     = {true,  true,  0, {0}},                          // LED-09
    [LED_PAT_PRESS]         = {false, false, 1, {100}},                        // LED-10
    [LED_PAT_HOLD_WAIT]     = {false, true,  0, {0}},                          // LED-11
    [LED_PAT_HOLD_WIFI]     = {true,  true,  2, {500, 500}},                   // LED-12
    [LED_PAT_HOLD_RESET]    = {true,  true,  2, {250, 250}},                   // LED-13
    [LED_PAT_REFUSED]       = {true,  false, 6, {100, 100, 100, 100, 100, 100}}, // LED-14
    [LED_PAT_ACCEPTED]      = {true,  false, 1, {600}},                        // LED-15
};

// LED level for a pattern elapsed_ms after it started. For a one-shot
// pattern, *done is set once it has finished (the level is then
// meaningless). done may be NULL.
static inline bool led_pattern_level(const led_pattern_t *p, uint32_t elapsed_ms, bool *done)
{
    if (done != NULL) {
        *done = false;
    }
    if (p->steps == 0) {
        return p->first_on;
    }
    uint32_t total = 0;
    for (uint8_t i = 0; i < p->steps; i++) {
        total += p->ms[i];
    }
    if (p->repeat) {
        elapsed_ms %= total;
    } else if (elapsed_ms >= total) {
        if (done != NULL) {
            *done = true;
        }
        return false;
    }
    bool level = p->first_on;
    for (uint8_t i = 0; i < p->steps; i++) {
        if (elapsed_ms < p->ms[i]) {
            return level;
        }
        elapsed_ms -= p->ms[i];
        level = !level;
    }
    return level;
}

#ifdef __cplusplus
}
#endif
