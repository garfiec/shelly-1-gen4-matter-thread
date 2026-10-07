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

// shelly_app.h
//
// Application plumbing shared by every variant: NVS bring-up, the Matter
// event and Identify callbacks, GeneralDiagnostics (boot reason, hardware
// faults), the liveness watchdog and OTA image confirmation.

#pragma once

#include <esp_err.h>
#include <esp_matter.h>

// Initialize NVS. If the partition is full or was written by a newer NVS
// format, erase it and initialize again, so the device comes back up
// ready to commission instead of boot-looping. Call first in app_main().
esp_err_t shelly_nvs_init(void);

// The Matter device event callback. Pass to esp_matter::start().
void shelly_app_event_cb(const chip::DeviceLayer::ChipDeviceEvent *event, intptr_t arg);

// The Identify callback. Pass to node::create(). Blinks the status LED.
esp_err_t shelly_app_identification_cb(esp_matter::identification::callback_type_t type, uint16_t endpoint_id,
                                       uint8_t effect_id, uint8_t effect_variant, void *priv_data);

// Add BootReason and ActiveHardwareFaults, and the HardwareFaultChange
// event, to GeneralDiagnostics on the root endpoint. Adds attributes
// only; creates no endpoint or cluster. Call after node::create().
esp_err_t shelly_diagnostics_add_attributes(esp_matter::node_t *node);

// Install the diagnostics provider that reports the boot reason and the
// hardware-fault registry. Must be called before esp_matter::start(): the
// GeneralDiagnostics boot event reads the provider at startup.
void shelly_diagnostics_install(void);

// Call right after esp_matter::start() succeeds. Publishes faults raised
// during boot and starts the health task: it feeds the task watchdog
// while the Matter thread stays responsive, and confirms a freshly
// installed OTA image after 60s of healthy running.
void shelly_app_started(void);

// True once shelly_app_started() has run.
bool shelly_app_is_started(void);

// Toggle or set the OnOff attribute on an endpoint. The attribute update
// runs the normal PRE_UPDATE path, which drives the relay. No-op before
// Matter starts or while endpoint_id is still 0. Takes the CHIP stack
// lock; do not call while holding the relay lock.
//
// shelly_onoff_toggle() is for local inputs (button, wall switch). It runs
// the local-toggle hook, if one is set, with the value it is about to flip,
// under the same lock as the read and the write.
void shelly_onoff_toggle(uint16_t endpoint_id);
void shelly_onoff_set(uint16_t endpoint_id, bool on);

// Called by shelly_onoff_toggle() before every local toggle, with the
// current OnOff value, while holding the CHIP stack lock. It must not
// block. Set during boot, before Matter starts.
typedef void (*shelly_local_toggle_hook_t)(bool currently_on);
void shelly_onoff_set_local_toggle_hook(shelly_local_toggle_hook_t hook);

// Set the OnOff attribute on the Matter thread, without waiting for the
// CHIP stack lock. For protection code that must never block on Matter.
void shelly_onoff_set_async(uint16_t endpoint_id, bool on);
