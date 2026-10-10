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


// device_profile.c

#include "device_profile.h"

#include <stddef.h>
#include <string.h>

static const char *const NAMES[DEVICE_PROFILE_COUNT] = {"outlet", "opener"};

const char *device_profile_name(device_profile_t profile)
{
    return (unsigned)profile < DEVICE_PROFILE_COUNT ? NAMES[profile] : "unknown";
}

bool device_profile_parse(const char *text, device_profile_t *out)
{
    if (text == NULL) {
        return false;
    }
    for (int p = 0; p < DEVICE_PROFILE_COUNT; p++) {
        if (strcmp(text, NAMES[p]) == 0) {
            *out = (device_profile_t)p;
            return true;
        }
    }
    return false;
}
