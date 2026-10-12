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



// device_profile.h
//
// The device type: what the relay is for, chosen on the Wi-Fi mode page on
// a build that offers more than one (CONFIG_SHELLY_TYPE_*). Each type has a
// Matter composition (its endpoints and device types). A change between two
// types with the same composition keeps the pairing; any other change needs
// the device to be paired again, which the next normal boot does by
// clearing its pairings (repair_decide()). Pure logic, host-tested.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Stored in NVS (shelly_cfg/profile): never reorder or reuse a value.
typedef enum {
    DEVICE_PROFILE_OUTLET = 0,       // On/Off Plug-in Unit, latching
    DEVICE_PROFILE_OPENER = 1,       // Garage door opener: a toggle is a pulse
    DEVICE_PROFILE_LIGHT = 2,        // On/Off Light, latching
    DEVICE_PROFILE_LIGHT_SWITCH = 3, // On/Off Light; SW drives bound lights
    DEVICE_PROFILE_COUNT,
} device_profile_t;

// One bit per type, for the set a build offers.
#define DEVICE_PROFILE_BIT(p) (1u << (unsigned)(p))

// The Matter composition of a type, stored in NVS (shelly_cfg/comp): never
// reorder or reuse a value. Metering is not part of it.
typedef enum {
    DEVICE_COMP_NONE = 0,
    DEVICE_COMP_PLUG = 1,          // Plug-in Unit, Temperature
    DEVICE_COMP_LIGHT = 2,         // On/Off Light, Temperature
    DEVICE_COMP_PLUG_CONTACT = 3,  // Plug-in Unit, Contact Sensor, Temperature
    DEVICE_COMP_LIGHT_SWITCH = 4,  // On/Off Light, Light Switch, Temperature, Contact Sensor
} device_comp_t;

// What the SW terminal does for a type.
typedef enum {
    DEVICE_SW_TOGGLE = 0,   // Switches the relay, in the wall switch modes
    DEVICE_SW_CONTACT,      // Door contact sensor (the 1 Gen4 Opener)
    DEVICE_SW_BINDING,      // Drives the bound lights (Light Switch)
} device_sw_role_t;

const char *device_profile_name(device_profile_t profile);
bool device_profile_parse(const char *text, device_profile_t *out);

// True if `offered` (DEVICE_PROFILE_BIT()s) includes `profile`.
bool device_profile_offered(uint32_t offered, device_profile_t profile);

// The composition and SW role of a type. `opener_contact`: the board's
// Opener has a door contact sensor on SW (the Shelly 1 Gen4); without it
// (a mains-powered board) the Opener has the Outlet's composition and its
// wall switch modes.
device_comp_t device_profile_comp(device_profile_t profile, bool opener_contact);
device_sw_role_t device_profile_sw_role(device_profile_t profile, bool opener_contact);

// True if changing from `from` to `to` needs the device to be paired again.
bool device_profile_needs_repair(device_profile_t from, device_profile_t to, bool opener_contact);

// At a normal boot, once Matter is up: what to do about the composition
// marker, the composition the device last ran with that its pairings were
// made for.
typedef enum {
    REPAIR_NOTHING = 0,     // The marker matches
    REPAIR_WRITE_MARKER,    // Record comp_now: nothing paired depends on the old one
    REPAIR_WAIT,            // Clear the pairings once the new firmware is confirmed
    REPAIR_RESET,           // Clear the pairings now (the marker is written after)
} repair_action_t;

// `marker_present`/`marker`: the stored marker. `paired`: any fabric, or a
// Thread network provisioned. `pending_verify`: the running image is a new
// one not yet confirmed (a restart now would roll it back).
repair_action_t repair_decide(bool marker_present, device_comp_t marker, device_comp_t comp_now, bool paired,
                              bool pending_verify);

#ifdef __cplusplus
}
#endif
