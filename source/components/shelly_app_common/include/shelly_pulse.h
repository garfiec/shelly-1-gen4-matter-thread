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



// shelly_pulse.h
//
// The garage door opener's momentary relay, for a board whose build offers
// opener mode (device_profile.h): a toggle closes the relay for
// CONFIG_SHELLY_PULSE_MS, then it opens again by itself. The same rules as
// the Shelly 1 Gen4 Opener's driver: the release is armed before the relay
// closes, so a relay that can't schedule its own release never closes.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

// The OnOff write's driver step (PRE_UPDATE) in opener mode. On: start a
// pulse, which ends by clearing OnOff again; refused before Matter is up,
// so a StartUpOnOff of On or Toggle never moves the door at boot. Off: end
// a pulse early.
esp_err_t shelly_pulse_write(uint16_t endpoint_id, bool on);

// At boot in opener mode: the relay and OnOff off, whatever was stored (a
// restart mid-pulse, or a change from outlet with the relay on).
esp_err_t shelly_pulse_set_defaults(uint16_t endpoint_id);

// The same pulse without Matter, for Wi-Fi maintenance mode: its own timer
// opens the relay directly. shelly_pulse_local_init() first; without it
// every pulse is refused. A toggle during a pulse ends it. True if the
// relay accepted it.
esp_err_t shelly_pulse_local_init(void);
bool shelly_pulse_local(void);
// Follow mode: on starts a pulse unless one is running; off ends one. True
// if the relay is now where it was asked to go (a pulse counts as on).
bool shelly_pulse_local_to(bool on);

#ifdef __cplusplus
}
#endif
