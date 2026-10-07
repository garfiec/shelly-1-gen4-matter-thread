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

// shelly_faults.h
//
// The hardware-fault registry. Every fault goes through here. It drives
// GeneralDiagnostics ActiveHardwareFaults, emits the HardwareFaultChange
// event and shows the status LED fault pattern.

#pragma once

#include <stdbool.h>

#include "shelly_fault_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

// Raise (active=true) or clear (active=false) a fault source. Safe to call
// from any task, before or after Matter starts; it never blocks on the
// CHIP stack lock. Changes after Matter starts are published on the
// Matter thread.
void shelly_fault_set(shelly_fault_src_t src, bool active);

#ifdef __cplusplus
}
#endif
