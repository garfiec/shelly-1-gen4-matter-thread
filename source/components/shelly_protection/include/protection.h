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

// protection.h
//
// Secondary electrical protection for metered boards (Shelly 1PM Gen4, and
// each channel of the Shelly 2PM Gen4). The home's circuit breaker is the
// primary protection. See docs/PROTECTION.md.
//
//   Overcurrent   I²t heat model on I_RMS, per relay, counted while that
//                 relay is closed. Trips and LATCHES that relay off. A
//                 deliberate On clears the latch: an On command, an
//                 OnWithTimedOff or OnWithRecallGlobalScene that the OnOff
//                 server acts on as an On, a Toggle while off, or a button
//                 press or wall-switch flip that turns the relay on. A power
//                 cycle also clears it. A software reset (crash, watchdog,
//                 OTA reboot) keeps it: the latch lives in RTC memory.
//   Total current On a board with a whole-device rating
//                 (CONFIG_SHELLY_PROT_TOTAL_RATED_CURRENT_MA, the 2PM's
//                 16 A), a second I²t model on the sum of the channels.
//                 Trips and latches EVERY relay off.
//   Overvoltage   Above the limit for the learned mains nominal, sustained.
//                 Trips every relay; restores each one that was on once the
//                 voltage has been back in range for 30s, unless the user
//                 switched that relay (Off, Toggle, button, wall switch)
//                 meanwhile.
//   Welded relay  Current flows although the relay is commanded off.
//                 Alert only.
//   Meter lost    No valid meter reading. Alert only; the relays keep working.
//
// After a trip, turn-on of the tripped relays is refused for a 2s cooldown,
// so a trip racing an in-flight On always ends with the relay off.

#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROTECTION_MAX_CHANNELS 2

// Restore the overcurrent latches from RTC memory (kept across software
// resets, cleared on power-on), load the learned mains nominal, and
// register the relay gate. Call after relay_init(), before Matter starts.
esp_err_t protection_init(void);

// True while an overcurrent trip is latched on relay 1 (or relay ch+1).
// When true at boot, the app forces that OnOff attribute off before
// Matter starts.
bool protection_is_latched(void);
bool protection_is_latched_ch(int ch);

// Set the OnOff endpoint protection turns off on a trip of relay ch+1,
// and hook its On, Off, Toggle and effect commands to tell deliberate
// user actions apart from internal writes. Also sets the local-toggle
// hooks (shelly_onoff_set_local_toggle_hook), so every button press or
// wall-switch flip counts: a toggle from Off that the relay accepts is a
// deliberate On (clears that relay's latch), and any toggle cancels that
// relay's pending overvoltage restore. Call after the endpoint is created,
// before Matter starts. protection_attach_endpoint() is channel 0.
esp_err_t protection_attach_endpoint(uint16_t relay_endpoint_id);
esp_err_t protection_attach_endpoint_ch(int ch, uint16_t relay_endpoint_id);

// Only the button and wall-switch hook of protection_attach_endpoint(), for
// Wi-Fi maintenance mode, which has no Matter endpoint: a local toggle that
// turns a relay on still clears its overcurrent trip, and any local toggle
// still cancels its overvoltage restore.
void protection_attach_local_inputs(void);

// One meter sample. valid=false when no valid reading arrived this poll.
// amps[ch] is relay ch+1's current; a meter that knows its reading clipped
// reports at least its full scale. Called from the metering task every poll.
typedef struct {
    bool valid;
    float volts;
    float amps[PROTECTION_MAX_CHANNELS];
    int64_t now_us;
} protection_sample_t;

void protection_feed(const protection_sample_t *sample);

#ifdef __cplusplus
}
#endif
