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

// ui_policy.c

#include "ui_policy.h"

#include <stddef.h>
#include <string.h>

static const char *const MODE_NAMES[UI_SWITCH_MODE_COUNT] = {
    [UI_SWITCH_MODE_TOGGLE] = "toggle",
    [UI_SWITCH_MODE_PUSH] = "push",
    [UI_SWITCH_MODE_FOLLOW] = "follow",
    [UI_SWITCH_MODE_DISABLED] = "disabled",
};

const char *ui_switch_mode_name(ui_switch_mode_t mode)
{
    return (unsigned)mode < UI_SWITCH_MODE_COUNT ? MODE_NAMES[mode] : "toggle";
}

bool ui_switch_mode_parse(const char *name, ui_switch_mode_t *out)
{
    if (name == NULL) {
        return false;
    }
    for (int m = 0; m < UI_SWITCH_MODE_COUNT; m++) {
        if (strcmp(name, MODE_NAMES[m]) == 0) {
            *out = (ui_switch_mode_t)m;
            return true;
        }
    }
    return false;
}

static ui_outcome_t decide_switch(const ui_state_t *st)
{
    switch (st->switch_role) {
    case UI_SWITCH_TOGGLE: {
        if (!st->maintenance && !st->started) {
            return UI_IGNORE;
        }
        bool local = st->maintenance;
        switch (st->switch_mode) {
        case UI_SWITCH_MODE_TOGGLE:
            return local ? UI_LOCAL_TOGGLE : UI_TOGGLE;
        case UI_SWITCH_MODE_PUSH:
            // A button changes twice per press: act on the press only.
            if (!st->switch_active) {
                return UI_IGNORE;
            }
            return local ? UI_LOCAL_TOGGLE : UI_TOGGLE;
        case UI_SWITCH_MODE_FOLLOW:
            if (st->switch_active) {
                return local ? UI_LOCAL_TURN_ON : UI_TURN_ON;
            }
            return local ? UI_LOCAL_TURN_OFF : UI_TURN_OFF;
        case UI_SWITCH_MODE_DISABLED:
        default:
            return UI_IGNORE;
        }
    }
    case UI_SWITCH_CONTACT:
    case UI_SWITCH_BINDING:
    default:
        // Both need Matter: no report or binding without it.
        return (!st->maintenance && st->started) ? UI_VARIANT : UI_IGNORE;
    }
}

ui_outcome_t ui_decide(ui_input_t input, const ui_state_t *st)
{
    if (st == NULL) {
        return UI_IGNORE;
    }
    switch (input) {
    case UI_INPUT_TAP:
    case UI_INPUT_TAP2:
        if (st->maintenance) {
            return UI_LOCAL_TOGGLE;
        }
        return st->started ? UI_TOGGLE : UI_REFUSE;

    case UI_INPUT_PAIRING:
        if (st->maintenance) {
            // Pairing needs Matter. In Wi-Fi mode the presses confirm a
            // change made on the page (setup code, switch mode), if one waits.
            return st->change_pending ? UI_CONFIRM_CHANGE : UI_REFUSE;
        }
        return st->started ? UI_OPEN_PAIRING : UI_REFUSE;

    case UI_INPUT_WIFI_MODE:
        if (st->maintenance) {
            return UI_EXIT_MAINTENANCE;
        }
        // A restart while the image is unconfirmed rolls the update back;
        // one during a Matter OTA download abandons it.
        if (!st->maintenance_available || !st->started || st->pending_verify || st->ota_busy) {
            return UI_REFUSE;
        }
        return UI_ENTER_MAINTENANCE;

    case UI_INPUT_FACTORY_RESET:
        // The reset restarts the device: refused for the same reason.
        if (st->pending_verify) {
            return UI_REFUSE;
        }
        if (st->maintenance || !st->started) {
            return UI_DEFER_RESET;
        }
        return UI_FACTORY_RESET;

    case UI_INPUT_SWITCH:
        return decide_switch(st);

    default:
        return UI_IGNORE;
    }
}

uint8_t ui_decide_channel(ui_input_t input, const ui_state_t *st)
{
    if (st == NULL) {
        return 0;
    }
    uint8_t channels = st->channels == 0 ? 1 : st->channels;
    switch (input) {
    case UI_INPUT_TAP2:
        return channels > 1 ? 1 : 0;
    case UI_INPUT_SWITCH:
        return st->switch_channel < channels ? st->switch_channel : 0;
    default:
        return 0;
    }
}
