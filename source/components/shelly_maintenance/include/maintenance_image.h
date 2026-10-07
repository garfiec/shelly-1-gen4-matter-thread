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

// maintenance_image.h
//
// Checks a firmware upload before any of it is written to flash. Pure C,
// no ESP-IDF includes, so the host tests can feed it good and bad files.
//
// The upload is the Matter .ota file, the same file Home Assistant serves
// for a Matter OTA update:
//
//   offset 0   Matter OTA fixed header: magic 0x1BEEF11E (u32 LE),
//              total file size (u64 LE), header TLV size (u32 LE)
//   offset 16  header TLV (vendor, product, version, digest; not parsed)
//   then       the ESP-IDF app image (the payload), which begins:
//     +0       esp_image_header_t (24 bytes): magic 0xE9, chip id at +12
//     +24      first segment header (8 bytes)
//     +32      esp_app_desc_t (256 bytes): magic 0xABCD5432, version at
//              +16 (32 chars), project name at +48 (32 chars)
//     +288     shelly_hw_desc_t: magic and the Shelly hardware code
//
// The hardware code is what keeps a 1PM image off a 1 Gen4 and the other
// way round. Different variants of the same hardware are allowed.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "shelly_hw_desc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MI_OTA_MAGIC            0x1BEEF11Eu
#define MI_OTA_FIXED_HEADER     16
#define MI_OTA_MAX_TLV          4096    // Far above any real header
#define MI_IMAGE_MAGIC          0xE9
#define MI_CHIP_ID_ESP32C6      0x000D
#define MI_APP_DESC_OFFSET      32
#define MI_APP_DESC_MAGIC       0xABCD5432u
#define MI_HW_DESC_OFFSET       288     // shelly_hw_desc_t (shelly_hw_desc.h)
#define MI_HW_CODE_LEN          SHELLY_HW_CODE_LEN

typedef enum {
    MI_NEED_MORE,       // Not enough bytes yet; feed at least need_bytes
    MI_OK,
    MI_NOT_OTA_FILE,    // A raw .bin or merged .bin: upload the .ota file
    MI_BAD_HEADER,      // Not a Matter OTA file, or a corrupt header
    MI_WRONG_CHIP,      // Not an ESP32-C6 image
    MI_NOT_AN_APP,      // No app description where it must be
    MI_NO_HW_CODE,      // Built before hardware codes existed
    MI_WRONG_HARDWARE,  // Built for a different Shelly
} mi_result_t;

typedef struct {
    size_t need_bytes;          // For MI_NEED_MORE
    uint64_t total_size;        // Whole .ota file
    size_t payload_offset;      // Where the app image starts
    uint64_t payload_size;
    char version[33];
    char project[33];           // The variant: light, outlet, opener, ...
    char hw_code[MI_HW_CODE_LEN + 1];
} mi_info_t;

// Check the first len bytes of an upload. Call again with more bytes while
// it returns MI_NEED_MORE.
mi_result_t mi_check(const uint8_t *buf, size_t len, const char *expect_hw_code, mi_info_t *info);

// A short user-facing explanation of a result.
const char *mi_result_text(mi_result_t r);

// Compare two "major.minor.patch" versions: <0, 0 or >0. Missing parts
// count as 0; anything after the numbers is ignored.
int mi_version_cmp(const char *a, const char *b);

#ifdef __cplusplus
}
#endif
