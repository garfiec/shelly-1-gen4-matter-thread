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


// shelly_hw_desc.h
//
// The Shelly hardware code, stored in every firmware image at a fixed
// offset (section .rodata_custom_desc, right after esp_app_desc_t, so byte
// 288 of the app image). Wi-Fi maintenance mode reads it from an upload to
// refuse firmware built for a different Shelly model, and
// scripts/make-matter-ota.py checks it before packaging. Pure C.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHELLY_HW_DESC_MAGIC    0x44574853u  // 'SHWD'
#define SHELLY_HW_CODE_LEN      16

typedef struct {
    uint32_t magic;
    char hw_code[SHELLY_HW_CODE_LEN];   // CONFIG_SHELLY_HARDWARE_CODE, NUL-padded
} shelly_hw_desc_t;

// This build's descriptor (shelly_app_common/shelly_hw_desc.c).
extern const shelly_hw_desc_t shelly_hw_desc;

#ifdef __cplusplus
}
#endif
