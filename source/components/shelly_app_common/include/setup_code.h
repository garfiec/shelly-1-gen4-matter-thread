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

// setup_code.h
//
// The device's Matter setup code: the passcode and discriminator it pairs
// with. Pure C, no ESP-IDF or CHIP includes, so the host tests cover it and
// the Wi-Fi maintenance page can use it with Matter off.
//
// The 11-digit manual code (Matter Core spec 5.1.4) carries the passcode
// and the top 4 bits of the discriminator, plus a Verhoeff check digit.
// Only the 11-digit form is accepted: the 21-digit form carries a vendor
// and product ID that would not match this firmware's.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The esp-matter test code every device used before per-device codes, and
// the fallback when a code can't be saved: 3497-011-2332.
#define SETUP_CODE_TEST_PASSCODE      20202021u
#define SETUP_CODE_TEST_DISCRIMINATOR 3840u

#define SETUP_CODE_PASSCODE_MAX       99999998u
#define SETUP_CODE_DISCRIMINATOR_MAX  0xFFFu

// 11 digits plus the terminator; formatted adds two dashes.
#define SETUP_CODE_MANUAL_LEN         12
#define SETUP_CODE_FORMATTED_LEN      14

typedef enum {
    SETUP_CODE_SOURCE_TEST = 0,      // The shared test code (fallback)
    SETUP_CODE_SOURCE_GENERATED = 1, // Made by the device
    SETUP_CODE_SOURCE_USER = 2,      // Entered by the owner (e.g. the Shelly sticker)
} setup_code_source_t;

typedef struct {
    uint32_t passcode;
    uint16_t discriminator;          // 12 bits
    uint8_t source;                  // setup_code_source_t
} setup_code_t;

typedef enum {
    SETUP_CODE_OK = 0,
    SETUP_CODE_ERR_FORMAT,           // Not 11 digits (after removing spaces and dashes)
    SETUP_CODE_ERR_CHECK_DIGIT,      // A digit is wrong or two are swapped
    SETUP_CODE_ERR_LONG_FORM,        // The 21-digit form (carries vendor and product ID)
    SETUP_CODE_ERR_PASSCODE,         // A passcode Matter doesn't allow
} setup_code_err_t;

// Matter's rule: 1..99999998, and none of 11111111..99999999 (repeated
// digits), 12345678 or 87654321.
bool setup_code_passcode_valid(uint32_t passcode);

// The 11-digit manual code, NUL-terminated. False if the passcode is not
// valid or the discriminator is over 12 bits.
bool setup_code_manual_encode(uint32_t passcode, uint16_t discriminator, char out[SETUP_CODE_MANUAL_LEN]);

// The same, as XXXX-XXX-XXXX.
bool setup_code_manual_format(uint32_t passcode, uint16_t discriminator, char out[SETUP_CODE_FORMATTED_LEN]);

// Parse a manual code as typed: digits, with any spaces and dashes. On
// success, sets the passcode and the top 4 bits of the discriminator.
setup_code_err_t setup_code_manual_parse(const char *text, uint32_t *passcode, uint8_t *short_discriminator);

// A random code. rand32 must be a good random source; it is called until
// it yields a valid passcode and a discriminator other than the test one.
typedef uint32_t (*setup_code_rand32_fn)(void *ctx);
void setup_code_generate(setup_code_rand32_fn rand32, void *ctx, setup_code_t *out);

// A full 12-bit discriminator for a code entered by hand: its top 4 bits
// from the code, the low 8 from random_low8.
uint16_t setup_code_discriminator_from_short(uint8_t short_discriminator, uint8_t random_low8);

// The saved record: magic, format version, passcode, discriminator, source.
#define SETUP_CODE_RECORD_LEN 12
void setup_code_record_pack(const setup_code_t *code, uint8_t out[SETUP_CODE_RECORD_LEN]);
// False for a record of the wrong size, magic or version, or with a code
// that is not valid.
bool setup_code_record_unpack(const uint8_t *in, size_t len, setup_code_t *out);

#ifdef __cplusplus
}
#endif
