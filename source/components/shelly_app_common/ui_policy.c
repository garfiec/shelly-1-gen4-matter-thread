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

static ui_outcome_t decide_switch(const ui_state_t *st)
{
    switch (st->switch_role) {
    case UI_SWITCH_TOGGLE:
        if (st->maintenance) {
            return UI_LOCAL_TOGGLE;
        }
        return st->started ? UI_TOGGLE : UI_IGNORE;
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
        if (st->maintenance) {
            return UI_LOCAL_TOGGLE;
        }
        return st->started ? UI_TOGGLE : UI_REFUSE;

    case UI_INPUT_PAIRING:
        // Pairing needs Matter.
        return (!st->maintenance && st->started) ? UI_OPEN_PAIRING : UI_REFUSE;

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
