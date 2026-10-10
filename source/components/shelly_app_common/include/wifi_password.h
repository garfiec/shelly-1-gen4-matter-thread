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


// wifi_password.h
//
// The Wi-Fi maintenance network's password: what one may be, and the
// default made from the sticker's setup code. Pure logic, host-tested.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// WPA2 and WPA3 passphrases: 8 to 63 printable ASCII characters.
#define WIFI_PASSWORD_MIN 8
#define WIFI_PASSWORD_MAX 63

bool wifi_password_valid(const char *s);

// The default password: the 11 digits of the manual setup code, without
// its dashes ("2058-491-0137" -> "20584910137"). False if text isn't one.
bool wifi_password_from_code_text(const char *text, char out[12]);

#ifdef __cplusplus
}
#endif
