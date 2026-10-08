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
#include <stdint.h>
#include <sdkconfig.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(CONFIG_SHELLY_CHANNELS) && CONFIG_SHELLY_CHANNELS > 1
#define SWITCH_INPUT_CHANNELS CONFIG_SHELLY_CHANNELS
#else
#define SWITCH_INPUT_CHANNELS 1
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
// What a change does depends on the variant and the wall-switch mode
// (docs/BUTTON-AND-LED.md, Wall switch modes): on_change passes it to
// shelly_ui_switch_changed(switch_input_active(level)).
//
// HARDWARE NOTE: The Shelly PCB has an external circuit that defines the
// SW pin level. Do not enable the internal pullup or pulldown — internal
// pulls will fight the external circuit and prevent the ISR from firing.
//
// Returns ESP_OK on success, error code on failure.
esp_err_t switch_input_init(switch_input_cb_t on_change, void *arg);

// The same for every input on a 2-channel board (SW1 is CONFIG_SHELLY_SWITCH_GPIO,
// SW2 CONFIG_SHELLY_SWITCH2_GPIO): on_change gets the input's channel, 0 for SW1.
// Use one of the two init functions, once.
typedef void (*switch_input_ch_cb_t)(int ch, bool level, void *arg);
esp_err_t switch_input_init_ch(switch_input_ch_cb_t on_change, void *arg);

// The current raw level of the SW pin (true = HIGH).
bool switch_input_get_level(void);
bool switch_input_get_level_ch(int ch);

// For diagnostics: the last settled level, and the raw edges the interrupt
// has seen since boot.
bool switch_input_stable_level_ch(int ch);
uint32_t switch_input_edge_count(int ch);

// Whether a level means the switch is on or pressed
// (CONFIG_SHELLY_SWITCH_ACTIVE_HIGH).
bool switch_input_active(bool level);

#ifdef __cplusplus
}
#endif
