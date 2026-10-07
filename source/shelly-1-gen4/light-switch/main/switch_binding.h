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

// switch_binding.h

#pragma once

#include <esp_err.h>
#include <stdbool.h>

// Detached wall switch on the SW terminal (GPIO10 on Shelly 1 Gen4).
//
// The shared switch_input module debounces the pin and reports each real
// change of level. Every flip then sends an OnOff Toggle command through
// the switch endpoint's Matter bindings, instead of driving the local
// relay. A standard wall switch toggles the bound device on every flip,
// regardless of position.
//
// Each change also publishes the settled toggle position to the Boolean
// State cluster on the contact sensor endpoint, so controllers can see
// the physical switch state that detached mode otherwise hides.
//
// Returns ESP_OK on success, error code on failure.
esp_err_t switch_binding_init(void);

// Read the current wall switch position as the Matter StateValue
// (true = toggle on/closed, false = off/open), applying
// SWITCH_ON_IS_HIGH. Used to seed the endpoint's initial value at boot.
bool switch_binding_read_state(void);

// Push the current switch position to the Boolean State cluster on the
// contact sensor endpoint. Called on each change, and once after Matter
// starts to publish the boot state. Takes the CHIP stack lock internally.
void switch_binding_report(void);
