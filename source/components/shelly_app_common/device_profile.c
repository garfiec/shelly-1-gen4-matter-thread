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



// device_profile.c

#include "device_profile.h"

#include <stddef.h>
#include <string.h>

static const char *const NAMES[DEVICE_PROFILE_COUNT] = {"outlet", "opener", "light", "light-switch"};

const char *device_profile_name(device_profile_t profile)
{
    return (unsigned)profile < DEVICE_PROFILE_COUNT ? NAMES[profile] : "unknown";
}

bool device_profile_parse(const char *text, device_profile_t *out)
{
    if (text == NULL) {
        return false;
    }
    for (int p = 0; p < DEVICE_PROFILE_COUNT; p++) {
        if (strcmp(text, NAMES[p]) == 0) {
            *out = (device_profile_t)p;
            return true;
        }
    }
    return false;
}

bool device_profile_offered(uint32_t offered, device_profile_t profile)
{
    return (unsigned)profile < DEVICE_PROFILE_COUNT && (offered & DEVICE_PROFILE_BIT(profile)) != 0;
}

device_comp_t device_profile_comp(device_profile_t profile, bool opener_contact)
{
    switch (profile) {
    case DEVICE_PROFILE_OUTLET:
        return DEVICE_COMP_PLUG;
    case DEVICE_PROFILE_OPENER:
        return opener_contact ? DEVICE_COMP_PLUG_CONTACT : DEVICE_COMP_PLUG;
    case DEVICE_PROFILE_LIGHT:
        return DEVICE_COMP_LIGHT;
    case DEVICE_PROFILE_LIGHT_SWITCH:
        return DEVICE_COMP_LIGHT_SWITCH;
    default:
        return DEVICE_COMP_NONE;
    }
}

device_sw_role_t device_profile_sw_role(device_profile_t profile, bool opener_contact)
{
    if (profile == DEVICE_PROFILE_OPENER && opener_contact) {
        return DEVICE_SW_CONTACT;
    }
    if (profile == DEVICE_PROFILE_LIGHT_SWITCH) {
        return DEVICE_SW_BINDING;
    }
    return DEVICE_SW_TOGGLE;
}

bool device_profile_needs_repair(device_profile_t from, device_profile_t to, bool opener_contact)
{
    return device_profile_comp(from, opener_contact) != device_profile_comp(to, opener_contact);
}

repair_action_t repair_decide(bool marker_present, device_comp_t marker, device_comp_t comp_now, bool paired,
                              bool pending_verify)
{
    // No marker: the first boot of this firmware, upgraded from a build of
    // one type (whose composition is the default type's by construction)
    // or freshly installed. Either way the pairings, if any, match.
    if (!marker_present || marker == comp_now) {
        return marker_present ? REPAIR_NOTHING : REPAIR_WRITE_MARKER;
    }
    // Nothing paired to the old composition (never paired, or the reset
    // already ran): record the new one. The marker changes only here, so
    // a power cut anywhere before just runs the reset again.
    if (!paired) {
        return REPAIR_WRITE_MARKER;
    }
    // The reset restarts the device, which would roll back a new image
    // that hasn't confirmed itself yet.
    return pending_verify ? REPAIR_WAIT : REPAIR_RESET;
}
