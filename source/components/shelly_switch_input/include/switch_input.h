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

// switch_input.h

#pragma once

#include <esp_err.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Called on the switch task each time the debounced SW level changes.
// level is the raw GPIO level (true = HIGH) after it settled.
typedef void (*switch_input_cb_t)(bool level, void *arg);

// Initialize the SW terminal input (CONFIG_SHELLY_SWITCH_GPIO).
//
// Configures the pin as an interrupt-driven input. On any edge it
// debounces for 50ms, reads the settled level and, only if that level
// differs from the last settled level, calls on_change(level, arg). A
// glitch that returns to the previous level is ignored.
//
// A toggle-mode wall switch toggles the relay from on_change on every
// flip, regardless of position.
//
// HARDWARE NOTE: The Shelly PCB has an external circuit that defines the
// SW pin level. Do not enable the internal pullup or pulldown — internal
// pulls will fight the external circuit and prevent the ISR from firing.
//
// Returns ESP_OK on success, error code on failure.
esp_err_t switch_input_init(switch_input_cb_t on_change, void *arg);

// The current raw level of the SW pin (true = HIGH).
bool switch_input_get_level(void);

#ifdef __cplusplus
}
#endif
