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
    SETUP_CODE_SOURCE_USER = 2,      // Entered by the owner
    SETUP_CODE_SOURCE_STICKER = 3,   // Shelly's own, from the stock "shelly" partition (never saved)
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

// ---- The QR code payload (Matter Core spec 5.1.3) ----
// "MT:" and the base38 text of 88 bits: version (3), vendor ID (16),
// product ID (16), commissioning flow (2), discovery capabilities (8),
// discriminator (12), passcode (27), padding (4). Optional TLV data may
// follow; it is ignored.

#define SETUP_CODE_QR_LEN 23 // "MT:" + 19 base38 characters + NUL (no TLV)
#define SETUP_CODE_QR_MAX 64 // The longest QR text accepted from the sticker, with NUL

typedef struct {
    uint8_t version;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t flow;            // 0: standard commissioning
    uint8_t capabilities;    // Bit 1: BLE, bit 2: on network
    uint16_t discriminator;
    uint32_t passcode;
} setup_code_qr_t;

// Decode "MT:..." text. False for anything that isn't a version 0 payload
// with a valid passcode.
bool setup_code_qr_decode(const char *text, setup_code_qr_t *out);
// Encode a payload without TLV data. False if it isn't valid.
bool setup_code_qr_encode(const setup_code_qr_t *in, char out[SETUP_CODE_QR_LEN]);

// ---- Shelly's sticker code, from the stock "shelly" partition ----
// Its JSON carries matter.setup_qr_code, matter.setup_manual_code and
// matter.setup_verifier ("<salt base64>,<iterations>,<verifier base64>").

#define SETUP_CODE_SALT_MAX     32 // Matter's SPAKE2+ salt: 16 to 32 bytes
#define SETUP_CODE_VERIFIER_LEN 97 // Serialized SPAKE2+ verifier (W0 and L)

typedef struct {
    setup_code_t code;       // Source SETUP_CODE_SOURCE_STICKER
    char qr[SETUP_CODE_QR_MAX];
    bool has_verifier;       // The stored salt, iterations and verifier below
    uint32_t iterations;
    uint8_t salt_len;
    uint8_t salt[SETUP_CODE_SALT_MAX];
    uint8_t verifier[SETUP_CODE_VERIFIER_LEN];
} setup_code_sticker_t;

// Check the sticker's QR and manual codes against each other: both must
// parse and carry the same passcode and short discriminator. The verifier
// text may be NULL; if it is malformed, has_verifier is false and the code
// is still used (the passcode is known; the device makes its own salt).
bool setup_code_sticker_parse(const char *qr, const char *manual, const char *verifier, setup_code_sticker_t *out);

// Does a typed manual code name the sticker's code?
bool setup_code_is_sticker(const setup_code_sticker_t *sticker, uint32_t passcode, uint8_t short_discriminator);

// The saved record: magic, format version, passcode, discriminator, source.
#define SETUP_CODE_RECORD_LEN 12
void setup_code_record_pack(const setup_code_t *code, uint8_t out[SETUP_CODE_RECORD_LEN]);
// False for a record of the wrong size, magic or version, or with a code
// that is not valid. A sticker code is never saved, so that source is
// refused too.
bool setup_code_record_unpack(const uint8_t *in, size_t len, setup_code_t *out);

#ifdef __cplusplus
}
#endif
