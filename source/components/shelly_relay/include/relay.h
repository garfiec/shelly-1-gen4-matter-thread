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

// relay.h

#pragma once

#include <esp_err.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Relay control for the mains-switching relay (CONFIG_SHELLY_RELAY_GPIO).
//
// This module owns the relay GPIO and is the single point of relay
// control in the firmware. All relay state changes from Matter
// commands, the onboard button, the wall switch input, thermal and
// electrical protection, or boot-time defaults go through relay_set()
// or relay_trip().
//
// SAFETY INVARIANT
// ----------------
// The relay shall de-energize (drive LOW) under all conditions where
// the firmware cannot guarantee the user's intended state:
//
//   1. PRE-INIT / BOOT: relay_init() drives the GPIO LOW immediately
//      after configuring it as an output, then releases any pad hold
//      the previous firmware left on the pin. A held pad ignores
//      writes, so without the release the relay could stay frozen in
//      the stock firmware's state after a conversion. The window
//      between chip reset and relay_init() is governed by the
//      hardware's reset state; the Shelly PCB pulls the driver input
//      low in this window, so the relay stays de-energized.
//
//   2. FAULTS: protection modules (thermal, electrical) register a
//      gate with relay_register_gate(). While any gate refuses,
//      relay_set(true) is refused. A fault that must open the relay
//      calls relay_trip(), which drives the pin low under the same
//      lock as the gate check, so an On that is already in flight
//      cannot re-energize the relay after the trip.
//
//   3. THREAD / NETWORK LOSS: the relay state is INTENTIONALLY held.
//      Loss of Thread connectivity does not change physical state.
//      Users should not lose control of their load because of a
//      network blip. It is not a safety failure.
//
//   4. UNHANDLED EXCEPTION / PANIC: the chip resets. On the next
//      boot, relay_init() re-enforces the safe-low invariant.
//
// CONTRIBUTOR CONVENTION
// ----------------------
// Any change that touches relay control must preserve these
// invariants. In particular:
//   - Do not add bypass paths to relay_set() that skip the gates.
//   - Do not remove or reorder the safe-low GPIO drive in relay_init().
//   - Do not add code paths that turn the relay ON without going
//     through relay_set().
//   - Gates must not block: they run with the relay lock held.
//   - Never call into Matter (attribute::update, the CHIP stack lock)
//     while holding the relay lock.
//
// Reviewers should reject pull requests that weaken these guarantees
// without explicit discussion of the safety implications.

// A turn-on gate. Returns NULL to allow turn-on, or a short reason
// string (for the log) to refuse it. Runs with the relay lock held, so
// it must only read state; it must not block or call into Matter.
typedef const char *(*relay_gate_fn_t)(void);

// Initialize the relay subsystem. Configures the relay GPIO as an
// output, drives it LOW and releases any pad hold. Must be called once
// during boot, before any other relay-related code runs.
//
// Returns ESP_OK on success, error code on failure. On failure the
// relay refuses every turn-on.
esp_err_t relay_init(void);

// Register a turn-on gate. Call during boot, before Matter starts.
// Returns ESP_ERR_NO_MEM if the gate table is full.
esp_err_t relay_register_gate(relay_gate_fn_t gate);

// Set the relay state.
//
// power=true:  Energize the relay (close the contacts, turn the load ON).
//              REFUSED with ESP_ERR_INVALID_STATE while any registered
//              gate refuses, or if relay_init() failed.
// power=false: De-energize the relay (open the contacts, turn the load OFF).
//              Always allowed. Turning off is never blocked.
//
// Safe to call from any task. Does not block on Matter.
esp_err_t relay_set(bool power);

// Force the relay off because of a fault. Equivalent to relay_set(false),
// but documents intent at the call site. The caller sets its own fault
// state (the state its gate reads) BEFORE calling this, so any turn-on
// that takes the relay lock afterwards is refused. The caller then
// updates the Matter OnOff attribute to Off, outside the relay lock.
void relay_trip(void);

// The level last written to the relay pin (true = energized).
bool relay_get(void);

#ifdef __cplusplus
}
#endif
