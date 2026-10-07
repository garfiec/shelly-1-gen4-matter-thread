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

// maintenance.h
//
// Wi-Fi maintenance mode: a boot with Matter and Thread off, in which the
// device runs its own open Wi-Fi network and a web page that installs a
// firmware update (the Matter .ota file).
//
// Entered by holding the button 5 s (shelly_ui.cpp records the request and
// restarts); each variant's app_main checks shelly_maintenance_boot_pending()
// right after NVS and the LED, and calls shelly_maintenance_run() instead of
// starting Matter. This mode is part of the device UI: docs/BUTTON-AND-LED.md
// and docs/UPDATING.md describe it; keep them in step (CLAUDE.md).

#pragma once

#include <shelly_app.h>

// Start the network, the captive-portal DNS and the web page, then wait.
// Restarts into normal mode after an update, on the page's Exit button,
// on the 5 s hold, or after CONFIG_SHELLY_MAINTENANCE_IDLE_TIMEOUT_S with
// the page closed. Never returns.
//
// Call after the relay, protection and inputs are up. relay_was_on is the
// relay state saved before the restart; it is restored through the relay
// gate. local_toggle backs the page's Toggle button.
[[noreturn]] void shelly_maintenance_run(bool relay_was_on, shelly_ui_toggle_fn_t local_toggle);
