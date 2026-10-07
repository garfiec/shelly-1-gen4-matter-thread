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

// button.h

#pragma once

#include <esp_err.h>

#include "button_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

// Called once per completed gesture (see button_logic.h), from the button
// task (not the esp_timer task), so it may take the CHIP stack lock.
typedef void (*shelly_button_action_cb_t)(button_action_t action, void *arg);

// Initialize the device button (CONFIG_SHELLY_BUTTON_GPIO).
//
// Recognizes taps, multi-presses and holds (button_logic.h), shows the
// press blip and the hold hints on the status LED, and calls on_action(arg)
// for each completed gesture. The button is the device's main UI: see
// docs/BUTTON-AND-LED.md.
esp_err_t button_init(shelly_button_action_cb_t on_action, void *arg);

#ifdef __cplusplus
}
#endif
