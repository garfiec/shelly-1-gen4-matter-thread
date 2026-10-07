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

#ifdef __cplusplus
extern "C" {
#endif

typedef void *shelly_button_handle_t;

// Called once per press, from the button task (not the esp_timer task),
// so it may take the CHIP stack lock and update attributes.
typedef void (*shelly_button_cb_t)(void *arg);

// Initialize the device button (CONFIG_SHELLY_BUTTON_GPIO).
//
// Configures the onboard button as an input with the variant's active
// level. On press-down, on_press(arg) runs on the button task.
//
// The returned handle can be passed to app_reset_button_register() to
// also wire factory-reset behavior to the same physical button.
//
// Returns a button handle on success, NULL on failure.
shelly_button_handle_t button_init(shelly_button_cb_t on_press, void *arg);

#ifdef __cplusplus
}
#endif
