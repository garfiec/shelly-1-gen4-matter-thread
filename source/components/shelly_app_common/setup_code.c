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

// setup_code.c

#include "setup_code.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECORD_MAGIC   0x444F4353u // 'SCOD'
#define RECORD_VERSION 1u

// Verhoeff check digit (the same algorithm as CHIP's Verhoeff10).
static const uint8_t k_mul[10][10] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}, {1, 2, 3, 4, 0, 6, 7, 8, 9, 5}, {2, 3, 4, 0, 1, 7, 8, 9, 5, 6},
    {3, 4, 0, 1, 2, 8, 9, 5, 6, 7}, {4, 0, 1, 2, 3, 9, 5, 6, 7, 8}, {5, 9, 8, 7, 6, 0, 4, 3, 2, 1},
    {6, 5, 9, 8, 7, 1, 0, 4, 3, 2}, {7, 6, 5, 9, 8, 2, 1, 0, 4, 3}, {8, 7, 6, 5, 9, 3, 2, 1, 0, 4},
    {9, 8, 7, 6, 5, 4, 3, 2, 1, 0},
};
static const uint8_t k_perm[8][10] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}, {1, 5, 7, 6, 2, 8, 3, 0, 9, 4}, {5, 8, 0, 3, 7, 9, 6, 1, 4, 2},
    {8, 9, 1, 6, 0, 4, 3, 5, 2, 7}, {9, 4, 5, 3, 1, 2, 6, 8, 7, 0}, {4, 2, 8, 6, 5, 7, 3, 9, 0, 1},
    {2, 7, 9, 3, 8, 0, 6, 4, 1, 5}, {7, 0, 4, 6, 9, 1, 3, 2, 5, 8},
};
static const uint8_t k_inv[10] = {0, 4, 3, 2, 1, 5, 6, 7, 8, 9};

static char verhoeff_check(const char *digits, size_t n)
{
    uint8_t c = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t d = (uint8_t)(digits[n - 1 - i] - '0');
        c = k_mul[c][k_perm[(i + 1) % 8][d]];
    }
    return (char)('0' + k_inv[c]);
}

// The low n decimal digits of v, zero-padded.
static void put_digits(char *out, unsigned v, size_t n)
{
    for (size_t i = n; i-- > 0;) {
        out[i] = (char)('0' + v % 10u);
        v /= 10u;
    }
}

bool setup_code_passcode_valid(uint32_t passcode)
{
    if (passcode == 0 || passcode > SETUP_CODE_PASSCODE_MAX) {
        return false;
    }
    if (passcode % 11111111u == 0) { // 11111111, 22222222 .. 88888888
        return false;
    }
    return passcode != 12345678u && passcode != 87654321u;
}

bool setup_code_manual_encode(uint32_t passcode, uint16_t discriminator, char out[SETUP_CODE_MANUAL_LEN])
{
    if (!setup_code_passcode_valid(passcode) || discriminator > SETUP_CODE_DISCRIMINATOR_MAX) {
        return false;
    }
    unsigned short_disc = (unsigned)(discriminator >> 8);
    unsigned chunk1 = short_disc >> 2;                                  // 1 digit; bit 2 (VID/PID) clear
    unsigned chunk2 = ((short_disc & 0x3u) << 14) | (passcode & 0x3FFFu); // 5 digits
    unsigned chunk3 = (unsigned)(passcode >> 14);                        // 4 digits
    put_digits(out, chunk1, 1);
    put_digits(out + 1, chunk2, 5);
    put_digits(out + 6, chunk3, 4);
    out[10] = verhoeff_check(out, 10);
    out[11] = '\0';
    return true;
}

bool setup_code_manual_format(uint32_t passcode, uint16_t discriminator, char out[SETUP_CODE_FORMATTED_LEN])
{
    char m[SETUP_CODE_MANUAL_LEN];
    if (!setup_code_manual_encode(passcode, discriminator, m)) {
        return false;
    }
    snprintf(out, SETUP_CODE_FORMATTED_LEN, "%.4s-%.3s-%.4s", m, m + 4, m + 7);
    return true;
}

static unsigned digits_value(const char *s, size_t n)
{
    unsigned v = 0;
    for (size_t i = 0; i < n; i++) {
        v = v * 10u + (unsigned)(s[i] - '0');
    }
    return v;
}

setup_code_err_t setup_code_manual_parse(const char *text, uint32_t *passcode, uint8_t *short_discriminator)
{
    if (text == NULL) {
        return SETUP_CODE_ERR_FORMAT;
    }
    char d[22];
    size_t n = 0;
    for (const char *p = text; *p != '\0'; p++) {
        if (*p == ' ' || *p == '-' || *p == '\t' || *p == '\r' || *p == '\n') {
            continue;
        }
        if (*p < '0' || *p > '9' || n >= sizeof(d) - 1) {
            return SETUP_CODE_ERR_FORMAT;
        }
        d[n++] = *p;
    }
    d[n] = '\0';
    if (n == 21 && d[0] >= '4' && d[0] <= '7') {
        return SETUP_CODE_ERR_LONG_FORM;
    }
    if (n != 11) {
        return SETUP_CODE_ERR_FORMAT;
    }
    if (verhoeff_check(d, 10) != d[10]) {
        return SETUP_CODE_ERR_CHECK_DIGIT;
    }
    unsigned chunk1 = (unsigned)(d[0] - '0');
    if (chunk1 >= 4u && chunk1 <= 7u) {
        return SETUP_CODE_ERR_LONG_FORM; // The VID/PID flag: only valid in the 21-digit form
    }
    if (chunk1 > 7u) {
        return SETUP_CODE_ERR_FORMAT;
    }
    unsigned chunk2 = digits_value(d + 1, 5);
    unsigned chunk3 = digits_value(d + 6, 4);
    if (chunk2 > 0xFFFFu || chunk3 > 0x1FFFu) {
        return SETUP_CODE_ERR_FORMAT;
    }
    uint32_t pc = ((uint32_t)chunk3 << 14) | (chunk2 & 0x3FFFu);
    if (!setup_code_passcode_valid(pc)) {
        return SETUP_CODE_ERR_PASSCODE;
    }
    *passcode = pc;
    *short_discriminator = (uint8_t)(((chunk1 & 0x3u) << 2) | (chunk2 >> 14));
    return SETUP_CODE_OK;
}

void setup_code_generate(setup_code_rand32_fn rand32, void *ctx, setup_code_t *out)
{
    uint32_t pc;
    do {
        // 2^27 covers the range; rejection keeps it uniform.
        pc = rand32(ctx) & 0x7FFFFFFu;
    } while (!setup_code_passcode_valid(pc));
    uint16_t disc;
    do {
        disc = (uint16_t)(rand32(ctx) & SETUP_CODE_DISCRIMINATOR_MAX);
    } while (disc == SETUP_CODE_TEST_DISCRIMINATOR);
    out->passcode = pc;
    out->discriminator = disc;
    out->source = SETUP_CODE_SOURCE_GENERATED;
}

uint16_t setup_code_discriminator_from_short(uint8_t short_discriminator, uint8_t random_low8)
{
    return (uint16_t)(((short_discriminator & 0xFu) << 8) | random_low8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void setup_code_record_pack(const setup_code_t *code, uint8_t out[SETUP_CODE_RECORD_LEN])
{
    put32(out, RECORD_MAGIC);
    out[4] = RECORD_VERSION;
    out[5] = code->source;
    out[6] = (uint8_t)code->discriminator;
    out[7] = (uint8_t)(code->discriminator >> 8);
    put32(out + 8, code->passcode);
}

bool setup_code_record_unpack(const uint8_t *in, size_t len, setup_code_t *out)
{
    if (in == NULL || len != SETUP_CODE_RECORD_LEN || get32(in) != RECORD_MAGIC || in[4] != RECORD_VERSION) {
        return false;
    }
    uint8_t source = in[5];
    uint16_t disc = (uint16_t)(in[6] | (in[7] << 8));
    uint32_t pc = get32(in + 8);
    if ((source != SETUP_CODE_SOURCE_GENERATED && source != SETUP_CODE_SOURCE_USER) ||
        disc > SETUP_CODE_DISCRIMINATOR_MAX || !setup_code_passcode_valid(pc)) {
        return false;
    }
    out->passcode = pc;
    out->discriminator = disc;
    out->source = source;
    return true;
}

// ---- The QR code payload ----

static const char k_base38[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-.";

#define QR_PREFIX      "MT:"
#define QR_PAYLOAD_LEN 11 // 88 bits

static int base38_value(char c)
{
    const char *p = strchr(k_base38, c);
    return (p == NULL || c == '\0') ? -1 : (int)(p - k_base38);
}

// Base38 text to bytes: 5 characters make 3 bytes, 4 make 2, 2 make 1.
// Returns the byte count, or -1.
static int base38_decode(const char *text, uint8_t *out, size_t out_len)
{
    size_t len = strlen(text), n = 0;
    for (size_t i = 0; i < len;) {
        size_t chars = len - i >= 5 ? 5 : len - i;
        size_t bytes = chars == 5 ? 3 : chars == 4 ? 2 : chars == 2 ? 1 : 0;
        if (bytes == 0 || n + bytes > out_len) {
            return -1;
        }
        uint32_t v = 0;
        for (size_t k = chars; k-- > 0;) {
            int d = base38_value(text[i + k]);
            if (d < 0) {
                return -1;
            }
            v = v * 38u + (uint32_t)d;
        }
        if (v >> (8 * bytes) != 0) {
            return -1;
        }
        for (size_t k = 0; k < bytes; k++) {
            out[n++] = (uint8_t)(v >> (8 * k));
        }
        i += chars;
    }
    return (int)n;
}

static size_t base38_encode(const uint8_t *in, size_t len, char *out)
{
    size_t n = 0;
    for (size_t i = 0; i < len;) {
        size_t bytes = len - i >= 3 ? 3 : len - i;
        size_t chars = bytes == 3 ? 5 : bytes == 2 ? 4 : 2;
        uint32_t v = 0;
        for (size_t k = 0; k < bytes; k++) {
            v |= (uint32_t)in[i + k] << (8 * k);
        }
        for (size_t k = 0; k < chars; k++) {
            out[n++] = k_base38[v % 38u];
            v /= 38u;
        }
        i += bytes;
    }
    out[n] = '\0';
    return n;
}

// The payload's fields, least significant bit first.
static uint32_t take_bits(const uint8_t *b, unsigned *pos, unsigned count)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < count; i++, (*pos)++) {
        v |= (uint32_t)((b[*pos / 8] >> (*pos % 8)) & 1u) << i;
    }
    return v;
}

static void put_bits(uint8_t *b, unsigned *pos, uint32_t v, unsigned count)
{
    for (unsigned i = 0; i < count; i++, (*pos)++) {
        if ((v >> i) & 1u) {
            b[*pos / 8] |= (uint8_t)(1u << (*pos % 8));
        }
    }
}

bool setup_code_qr_decode(const char *text, setup_code_qr_t *out)
{
    if (text == NULL || strncmp(text, QR_PREFIX, 3) != 0) {
        return false;
    }
    uint8_t b[(SETUP_CODE_QR_MAX * 3) / 5 + 3];
    int n = base38_decode(text + 3, b, sizeof(b));
    if (n < QR_PAYLOAD_LEN) {
        return false;
    }
    unsigned pos = 0;
    setup_code_qr_t q;
    q.version = (uint8_t)take_bits(b, &pos, 3);
    q.vendor_id = (uint16_t)take_bits(b, &pos, 16);
    q.product_id = (uint16_t)take_bits(b, &pos, 16);
    q.flow = (uint8_t)take_bits(b, &pos, 2);
    q.capabilities = (uint8_t)take_bits(b, &pos, 8);
    q.discriminator = (uint16_t)take_bits(b, &pos, 12);
    q.passcode = take_bits(b, &pos, 27);
    if (q.version != 0 || !setup_code_passcode_valid(q.passcode)) {
        return false;
    }
    *out = q;
    return true;
}

bool setup_code_qr_encode(const setup_code_qr_t *in, char out[SETUP_CODE_QR_LEN])
{
    if (in->version != 0 || in->flow > 3u || in->discriminator > SETUP_CODE_DISCRIMINATOR_MAX ||
        !setup_code_passcode_valid(in->passcode)) {
        return false;
    }
    uint8_t b[QR_PAYLOAD_LEN] = {0};
    unsigned pos = 0;
    put_bits(b, &pos, in->version, 3);
    put_bits(b, &pos, in->vendor_id, 16);
    put_bits(b, &pos, in->product_id, 16);
    put_bits(b, &pos, in->flow, 2);
    put_bits(b, &pos, in->capabilities, 8);
    put_bits(b, &pos, in->discriminator, 12);
    put_bits(b, &pos, in->passcode, 27);
    memcpy(out, QR_PREFIX, 3);
    base38_encode(b, sizeof(b), out + 3);
    return true;
}

// ---- The sticker code ----

static int base64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// Standard base64 of exactly len characters (padded). Returns the byte
// count, or -1.
static int base64_decode(const char *text, size_t len, uint8_t *out, size_t out_len)
{
    if (len == 0 || len % 4 != 0) {
        return -1;
    }
    size_t n = 0;
    for (size_t i = 0; i < len; i += 4) {
        uint32_t v = 0;
        int pad = 0;
        for (size_t k = 0; k < 4; k++) {
            char c = text[i + k];
            int d;
            if (c == '=' && i + 4 == len && k >= 2) {
                d = 0;
                pad++;
            } else if (pad != 0 || (d = base64_value(c)) < 0) {
                return -1;
            }
            v = (v << 6) | (uint32_t)d;
        }
        size_t bytes = 3u - (size_t)pad;
        if (n + bytes > out_len) {
            return -1;
        }
        for (size_t k = 0; k < bytes; k++) {
            out[n++] = (uint8_t)(v >> (16 - 8 * k));
        }
    }
    return (int)n;
}

// "<salt base64>,<iterations>,<verifier base64>"
static bool parse_verifier(const char *text, setup_code_sticker_t *out)
{
    const char *c1 = strchr(text, ',');
    const char *c2 = c1 != NULL ? strchr(c1 + 1, ',') : NULL;
    if (c2 == NULL || strchr(c2 + 1, ',') != NULL) {
        return false;
    }
    int salt_len = base64_decode(text, (size_t)(c1 - text), out->salt, sizeof(out->salt));
    if (salt_len < 16) {
        return false;
    }
    char *end = NULL;
    unsigned long iterations = strtoul(c1 + 1, &end, 10);
    if (end != c2 || c1 + 1 == c2 || iterations < 1000u || iterations > 100000u) {
        return false;
    }
    if (base64_decode(c2 + 1, strlen(c2 + 1), out->verifier, sizeof(out->verifier)) != SETUP_CODE_VERIFIER_LEN) {
        return false;
    }
    out->salt_len = (uint8_t)salt_len;
    out->iterations = (uint32_t)iterations;
    return true;
}

bool setup_code_sticker_parse(const char *qr, const char *manual, const char *verifier, setup_code_sticker_t *out)
{
    setup_code_qr_t q;
    uint32_t passcode;
    uint8_t short_disc;
    if (qr == NULL || strlen(qr) >= SETUP_CODE_QR_MAX || !setup_code_qr_decode(qr, &q) ||
        setup_code_manual_parse(manual, &passcode, &short_disc) != SETUP_CODE_OK || passcode != q.passcode ||
        short_disc != (q.discriminator >> 8)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->code.passcode = q.passcode;
    out->code.discriminator = q.discriminator;
    out->code.source = SETUP_CODE_SOURCE_STICKER;
    strcpy(out->qr, qr);
    out->has_verifier = verifier != NULL && parse_verifier(verifier, out);
    if (!out->has_verifier) {
        out->iterations = 0;
        out->salt_len = 0;
    }
    return true;
}

bool setup_code_is_sticker(const setup_code_sticker_t *sticker, uint32_t passcode, uint8_t short_discriminator)
{
    return sticker != NULL && sticker->code.passcode == passcode &&
           (sticker->code.discriminator >> 8) == short_discriminator;
}
