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

// button_logic.c

#include "button_logic.h"

#include <string.h>

void button_logic_init(button_logic_t *b)
{
    memset(b, 0, sizeof(*b));
}

static button_action_t taps_action(uint8_t taps)
{
    return taps >= BUTTON_PAIRING_TAPS ? BUTTON_ACTION_PAIRING : BUTTON_ACTION_TOGGLE;
}

static button_action_t hold_action(uint32_t held_ms)
{
    if (held_ms >= BUTTON_HOLD_CANCEL_MS) {
        return BUTTON_ACTION_NONE;
    }
    if (held_ms >= BUTTON_HOLD_RESET_MS) {
        return BUTTON_ACTION_FACTORY_RESET;
    }
    if (held_ms >= BUTTON_HOLD_WIFI_MS) {
        return BUTTON_ACTION_WIFI_MODE;
    }
    return BUTTON_ACTION_NONE;
}

button_action_t button_logic_update(button_logic_t *b, bool pressed, uint32_t now_ms)
{
    button_action_t action = BUTTON_ACTION_NONE;

    // Taps whose gap ran out before this call (a late tick, or a press that
    // came after the gap) complete first.
    if (!b->pressed && b->taps > 0 && (uint32_t)(now_ms - b->release_ms) >= BUTTON_MULTI_GAP_MS) {
        action = taps_action(b->taps);
        b->taps = 0;
    }

    if (pressed && !b->pressed) {
        b->pressed = true;
        b->press_ms = now_ms;
    } else if (!pressed && b->pressed) {
        b->pressed = false;
        uint32_t held = now_ms - b->press_ms;
        if (held < BUTTON_TAP_MAX_MS) {
            if (b->taps < UINT8_MAX) {
                b->taps++;
            }
            b->release_ms = now_ms;
        } else {
            // A hold ends any tap sequence it interrupted.
            b->taps = 0;
            action = hold_action(held);
        }
    }
    return action;
}

button_hint_t button_logic_hint(const button_logic_t *b, uint32_t now_ms)
{
    if (!b->pressed) {
        return BUTTON_HINT_NONE;
    }
    uint32_t held = now_ms - b->press_ms;
    if (held < BUTTON_TAP_MAX_MS) {
        return BUTTON_HINT_NONE;
    }
    switch (hold_action(held)) {
    case BUTTON_ACTION_WIFI_MODE:
        return BUTTON_HINT_WIFI;
    case BUTTON_ACTION_FACTORY_RESET:
        return BUTTON_HINT_RESET;
    default:
        return held >= BUTTON_HOLD_CANCEL_MS ? BUTTON_HINT_NONE : BUTTON_HINT_WAIT;
    }
}

bool button_logic_busy(const button_logic_t *b)
{
    return b->pressed || b->taps > 0;
}
