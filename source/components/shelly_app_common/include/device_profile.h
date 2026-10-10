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
// What the relay is for, on a board whose build offers a choice
// (CONFIG_SHELLY_HAS_OPENER_MODE): an outlet that stays on or off, or a
// garage door opener that closes it for a short pulse. Chosen on the Wi-Fi
// mode page; the Matter composition is the same for both. Pure logic,
// host-tested.

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DEVICE_PROFILE_OUTLET = 0,  // Latching: the default
    DEVICE_PROFILE_OPENER,      // Momentary: a toggle is a pulse
    DEVICE_PROFILE_COUNT,
} device_profile_t;

// "outlet" or "opener", as the page and the log use them.
const char *device_profile_name(device_profile_t profile);
// The reverse; false if the text is none of them.
bool device_profile_parse(const char *text, device_profile_t *out);

#ifdef __cplusplus
}
#endif
