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
// Secondary electrical protection for metered boards (Shelly 1PM Gen4).
// The home's circuit breaker is the primary protection. See
// docs/PROTECTION.md.
//
//   Overcurrent   I²t heat model on I_RMS. Trips and LATCHES the relay off.
//                 A deliberate On clears the latch: an On/OnWithTimedOff/
//                 OnWithRecallGlobalScene command, or a Toggle, button press
//                 or wall-switch flip while off. A power cycle also clears
//                 it. A software reset (crash, watchdog, OTA reboot) keeps
//                 it: the latch lives in RTC memory.
//   Overvoltage   Above the limit for the learned mains nominal, sustained.
//                 Trips; restores the pre-trip state once the voltage has
//                 been back in range for 30s, unless the user switched the
//                 outlet (Off, Toggle, button, wall switch) meanwhile.
//   Welded relay  Current flows although the relay is commanded off.
//                 Alert only.
//   Meter lost    No valid meter frame. Alert only; the relay keeps working.
//
// After any trip, turn-on is refused for a 2s cooldown, so a trip racing
// an in-flight On always ends with the relay off.

#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Restore the overcurrent latch from RTC memory (kept across software
// resets, cleared on power-on), load the learned mains nominal, and
// register the relay gate. Call after relay_init(), before Matter starts.
esp_err_t protection_init(void);

// True while an overcurrent trip is latched. When true at boot, the app
// forces the OnOff attribute off before Matter starts.
bool protection_is_latched(void);

// Set the OnOff endpoint protection turns off on a trip, and hook its On,
// Off, Toggle and effect commands to tell deliberate user actions apart
// from internal writes. Also sets the shelly_onoff_toggle() local-toggle
// hook, so every button press or wall-switch flip counts: a toggle from Off
// is a deliberate On (clears the latch), and any toggle cancels a pending
// overvoltage restore. Call after the endpoint is created, before Matter
// starts.
esp_err_t protection_attach_endpoint(uint16_t relay_endpoint_id);

// One meter sample. valid=false when no valid frame arrived this poll.
// Called from the metering task every poll.
typedef struct {
    bool valid;
    float volts;
    float amps;
    bool amps_clipped;
    int64_t now_us;
} protection_sample_t;

void protection_feed(const protection_sample_t *sample);

#ifdef __cplusplus
}
#endif
