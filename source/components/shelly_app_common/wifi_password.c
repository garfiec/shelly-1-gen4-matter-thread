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


// wifi_password.c

#include "wifi_password.h"

#include <string.h>

bool wifi_password_valid(const char *s)
{
    if (s == NULL) {
        return false;
    }
    size_t n = strlen(s);
    if (n < WIFI_PASSWORD_MIN || n > WIFI_PASSWORD_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c > 0x7E) {
            return false;
        }
    }
    return true;
}

bool wifi_password_from_code_text(const char *text, char out[12])
{
    size_t n = 0;
    for (; *text != '\0'; text++) {
        if (*text == '-') {
            continue;
        }
        if (*text < '0' || *text > '9' || n == 11) {
            return false;
        }
        out[n++] = *text;
    }
    out[n] = '\0';
    return n == 11;
}
