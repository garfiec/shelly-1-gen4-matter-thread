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

// shelly_hw_snapshot.h
//
// Optional hardware snapshot (CONFIG_SHELLY_HW_SNAPSHOT, off by default; on
// for boards being brought up, like the 2PM). During a normal boot it
// samples the relay and input pins, the button, the temperature sensors,
// the heap and what the power-meter probe read, into memory that survives
// the restart into Wi-Fi mode, where the page shows it (/hw.json). A line a
// minute also goes to the diagnostics log. A power cut clears it.
//
// Nothing here runs before Matter starts, and nothing writes to hardware:
// it only reads pins and copies values other modules already hold.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sdkconfig.h>

#include "hw_snapshot.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_SHELLY_HW_SNAPSHOT

// Normal boot, after shelly_app_started(): take over the snapshot from the
// previous boot and start sampling every 10 s. After a crash restart (panic
// or watchdog) it starts nothing and keeps the crashed boot's snapshot, and
// the meter probe stays off: neither can turn a crash into a crash loop.
void shelly_hw_snapshot_start(void);

// For the power-meter probe (normal boot only).
// True if the probe must not run: this boot follows a crash restart, or a
// crash since power-on happened while the probe talked to the chip.
bool shelly_hw_snapshot_ade_blocked(void);
// Set or clear the guard that marks the probe as talking to the chip.
void shelly_hw_snapshot_ade_guard(bool on);
void shelly_hw_snapshot_ade_state(hw_ade_state_t state, int32_t err);
void shelly_hw_snapshot_ade_reg(uint16_t addr, uint8_t width, bool ok, uint32_t value);
// The full dump finished (dump true) or another live read did.
void shelly_hw_snapshot_ade_cycle_done(bool dump);
// A register stored so far.
bool shelly_hw_snapshot_ade_get(uint16_t addr, uint32_t *value);

// Wi-Fi mode: the JSON for /hw.json, the snapshot of the last normal boot
// with what the pins read now. Returns the length needed (like snprintf).
size_t shelly_hw_snapshot_json(char *buf, size_t len);

#else

static inline void shelly_hw_snapshot_start(void) {}

#endif

#ifdef __cplusplus
}
#endif
