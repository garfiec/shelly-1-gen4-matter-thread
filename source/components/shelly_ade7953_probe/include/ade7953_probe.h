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


// ade7953_probe.h
//
// Optional read-only probe of the ADE7953 power-meter chip
// (CONFIG_SHELLY_ADE7953_PROBE), for bringing up a board before it has
// metering. It starts CONFIG_SHELLY_ADE7953_PROBE_DELAY_S after Matter is
// up, reads the chip's identity, configuration and measurement registers
// once, then the measurements every minute, into the hardware snapshot
// (shelly_hw_snapshot.h) and the log.
//
// It never writes a register, never reads one whose read changes the chip
// (ade7953_regs.h), and leaves the chip's reset and interrupt pins alone.
// Every bus operation has a short timeout, and a crash while it talks to
// the chip stops it until the next power cycle.

#pragma once

#include <sdkconfig.h>

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_SHELLY_ADE7953_PROBE
// Normal boot, after shelly_hw_snapshot_start().
void ade7953_probe_start(void);
#else
static inline void ade7953_probe_start(void) {}
#endif

#ifdef __cplusplus
}
#endif
