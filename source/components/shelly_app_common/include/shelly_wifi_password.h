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


// shelly_wifi_password.h
//
// The Wi-Fi maintenance network's password. First match wins:
//   1. The owner's own, set on the page and saved with 3 presses of the
//      button (docs/BUTTON-AND-LED.md, B-26). Saved in NVS (namespace
//      "shelly_wifi"); a factory reset forgets it.
//   2. The setup code on the Shelly sticker, as 11 digits without dashes.
//   3. None: an open network, when the device has no sticker code.
// So after a factory reset the password is printed on the device, or there
// is none: Wi-Fi mode is the way to reflash, and must never be locked by a
// password nobody can find.

#pragma once

#include <stdbool.h>

#include <wifi_password.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SHELLY_WIFI_OPEN = 0,
    SHELLY_WIFI_STICKER,
    SHELLY_WIFI_OWN,
} shelly_wifi_security_t;

// What the network uses the next time Wi-Fi mode starts, and its password
// in out (empty when open). Never log out.
shelly_wifi_security_t shelly_wifi_password_choose(char out[WIFI_PASSWORD_MAX + 1]);

// Save the owner's own password, or forget it with "". False if it isn't
// valid or couldn't be saved.
bool shelly_wifi_password_save(const char *password);

// A factory reset: forget the owner's own password.
void shelly_wifi_password_factory_reset(void);

#ifdef __cplusplus
}
#endif
