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

// test_setup_code.c
//
// Host tests for the Matter setup code (setup_code.c): the 11-digit manual
// code, its check digit, the passcode rules, generation and the saved
// record. Build and run with test_host/run.sh.
//
// With an argument "vectors N", prints N random passcode/discriminator
// pairs and their manual codes instead, for the cross-check against CHIP's
// own SetupPayload.py (see docs/BUILDING.md, Repository structure).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "setup_code.h"

static int s_failures = 0;
static int s_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        s_checks++;                                                              \
        if (!(cond)) {                                                           \
            s_failures++;                                                        \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
        }                                                                        \
    } while (0)

// A deterministic source: xorshift32.
static uint32_t xorshift(void *ctx)
{
    uint32_t *s = (uint32_t *)ctx;
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

// Returns the values in turn, then repeats the last.
typedef struct {
    const uint32_t *v;
    size_t n, i;
} seq_t;

static uint32_t seq(void *ctx)
{
    seq_t *s = (seq_t *)ctx;
    uint32_t v = s->v[s->i < s->n ? s->i : s->n - 1];
    s->i++;
    return v;
}

static void test_passcode_rules(void)
{
    CHECK(!setup_code_passcode_valid(0));
    CHECK(setup_code_passcode_valid(1));
    CHECK(setup_code_passcode_valid(SETUP_CODE_PASSCODE_MAX));
    CHECK(!setup_code_passcode_valid(SETUP_CODE_PASSCODE_MAX + 1));
    CHECK(!setup_code_passcode_valid(99999999));
    for (uint32_t k = 1; k <= 8; k++) {
        CHECK(!setup_code_passcode_valid(11111111u * k));
    }
    CHECK(!setup_code_passcode_valid(12345678));
    CHECK(!setup_code_passcode_valid(87654321));
    CHECK(setup_code_passcode_valid(SETUP_CODE_TEST_PASSCODE));
    CHECK(setup_code_passcode_valid(12345679));
}

static void test_known_vector(void)
{
    char m[SETUP_CODE_MANUAL_LEN];
    char f[SETUP_CODE_FORMATTED_LEN];
    // The esp-matter test code printed in docs/COMMISSIONING.md.
    CHECK(setup_code_manual_encode(SETUP_CODE_TEST_PASSCODE, SETUP_CODE_TEST_DISCRIMINATOR, m));
    CHECK(strcmp(m, "34970112332") == 0);
    CHECK(setup_code_manual_format(SETUP_CODE_TEST_PASSCODE, SETUP_CODE_TEST_DISCRIMINATOR, f));
    CHECK(strcmp(f, "3497-011-2332") == 0);

    uint32_t pc = 0;
    uint8_t sd = 0xFF;
    CHECK(setup_code_manual_parse("3497-011-2332", &pc, &sd) == SETUP_CODE_OK);
    CHECK(pc == SETUP_CODE_TEST_PASSCODE);
    CHECK(sd == (SETUP_CODE_TEST_DISCRIMINATOR >> 8));
    CHECK(setup_code_manual_parse(" 3497 011 2332\n", &pc, &sd) == SETUP_CODE_OK);
    CHECK(setup_code_manual_parse("34970112332", &pc, &sd) == SETUP_CODE_OK);

    CHECK(!setup_code_manual_encode(0, 0, m));
    CHECK(!setup_code_manual_encode(1, 0x1000, m));
}

static void test_parse_rejects(void)
{
    uint32_t pc = 7;
    uint8_t sd = 7;
    CHECK(setup_code_manual_parse(NULL, &pc, &sd) == SETUP_CODE_ERR_FORMAT);
    CHECK(setup_code_manual_parse("", &pc, &sd) == SETUP_CODE_ERR_FORMAT);
    CHECK(setup_code_manual_parse("3497-011-233", &pc, &sd) == SETUP_CODE_ERR_FORMAT);
    CHECK(setup_code_manual_parse("3497-011-23321", &pc, &sd) == SETUP_CODE_ERR_FORMAT);
    CHECK(setup_code_manual_parse("3497-O11-2332", &pc, &sd) == SETUP_CODE_ERR_FORMAT);
    CHECK(setup_code_manual_parse("MT:Y.K9042C00KA0648G00", &pc, &sd) == SETUP_CODE_ERR_FORMAT);
    CHECK(setup_code_manual_parse("12345678901234567890123", &pc, &sd) == SETUP_CODE_ERR_FORMAT);
    // Nothing is written on failure.
    CHECK(pc == 7 && sd == 7);

    // Every single-digit change and every adjacent swap is caught.
    const char *good = "34970112332";
    char t[12];
    for (int i = 0; i < 11; i++) {
        for (char c = '0'; c <= '9'; c++) {
            if (c == good[i]) {
                continue;
            }
            memcpy(t, good, 12);
            t[i] = c;
            setup_code_err_t e = setup_code_manual_parse(t, &pc, &sd);
            CHECK(e != SETUP_CODE_OK);
        }
    }
    for (int i = 0; i < 10; i++) {
        if (good[i] == good[i + 1]) {
            continue;
        }
        memcpy(t, good, 12);
        t[i] = good[i + 1];
        t[i + 1] = good[i];
        CHECK(setup_code_manual_parse(t, &pc, &sd) != SETUP_CODE_OK);
    }

    // The 21-digit form, and an 11-digit code with the VID/PID flag set.
    CHECK(setup_code_manual_parse("749701123365521327694", &pc, &sd) == SETUP_CODE_ERR_LONG_FORM);
    // "7497011233" with a correct check digit: the flag is set.
    char flagged[12] = "7497011233";
    for (char c = '0'; c <= '9'; c++) {
        flagged[10] = c;
        flagged[11] = '\0';
        setup_code_err_t e = setup_code_manual_parse(flagged, &pc, &sd);
        CHECK(e == SETUP_CODE_ERR_LONG_FORM || e == SETUP_CODE_ERR_CHECK_DIGIT);
    }

    // A well-formed code whose passcode Matter forbids (12345678).
    char bad[SETUP_CODE_MANUAL_LEN];
    // Encode bypasses the check here by building it the long way.
    uint32_t forbidden = 12345678u;
    snprintf(bad, sizeof(bad), "%01u%05u%04u", 0u, (unsigned)(forbidden & 0x3FFF), (unsigned)(forbidden >> 14));
    char with_check[SETUP_CODE_MANUAL_LEN];
    int found = 0;
    for (char c = '0'; c <= '9' && !found; c++) {
        memcpy(with_check, bad, 10);
        with_check[10] = c;
        with_check[11] = '\0';
        setup_code_err_t e = setup_code_manual_parse(with_check, &pc, &sd);
        if (e != SETUP_CODE_ERR_CHECK_DIGIT) {
            CHECK(e == SETUP_CODE_ERR_PASSCODE);
            found = 1;
        }
    }
    CHECK(found);
}

static void test_round_trip(void)
{
    uint32_t state = 0x12345678u;
    for (int i = 0; i < 2000; i++) {
        setup_code_t c;
        setup_code_generate(xorshift, &state, &c);
        CHECK(setup_code_passcode_valid(c.passcode));
        CHECK(c.discriminator <= SETUP_CODE_DISCRIMINATOR_MAX);
        CHECK(c.discriminator != SETUP_CODE_TEST_DISCRIMINATOR);
        CHECK(c.source == SETUP_CODE_SOURCE_GENERATED);

        char f[SETUP_CODE_FORMATTED_LEN];
        CHECK(setup_code_manual_format(c.passcode, c.discriminator, f));
        uint32_t pc = 0;
        uint8_t sd = 0;
        CHECK(setup_code_manual_parse(f, &pc, &sd) == SETUP_CODE_OK);
        CHECK(pc == c.passcode);
        CHECK(sd == (c.discriminator >> 8));
        CHECK(setup_code_discriminator_from_short(sd, (uint8_t)c.discriminator) == c.discriminator);
    }
}

static void test_generate_rejects(void)
{
    // Invalid passcodes and the test discriminator are drawn again.
    const uint32_t v[] = {0, 12345678u, 0x7FFFFFFu, 42u, SETUP_CODE_TEST_DISCRIMINATOR, 0x1000u | 0x123u};
    seq_t s = {v, sizeof(v) / sizeof(v[0]), 0};
    setup_code_t c;
    setup_code_generate(seq, &s, &c);
    CHECK(c.passcode == 42u);
    CHECK(c.discriminator == 0x123u);
    CHECK(s.i == 6);
}

static void test_record(void)
{
    setup_code_t in = {.passcode = 20202022u, .discriminator = 0xABC, .source = SETUP_CODE_SOURCE_USER};
    uint8_t r[SETUP_CODE_RECORD_LEN];
    setup_code_record_pack(&in, r);
    setup_code_t out;
    CHECK(setup_code_record_unpack(r, sizeof(r), &out));
    CHECK(out.passcode == in.passcode && out.discriminator == in.discriminator && out.source == in.source);

    CHECK(!setup_code_record_unpack(NULL, sizeof(r), &out));
    CHECK(!setup_code_record_unpack(r, sizeof(r) - 1, &out));
    uint8_t bad[SETUP_CODE_RECORD_LEN];
    memcpy(bad, r, sizeof(r));
    bad[0] ^= 1; // magic
    CHECK(!setup_code_record_unpack(bad, sizeof(bad), &out));
    memcpy(bad, r, sizeof(r));
    bad[4] = 2; // version
    CHECK(!setup_code_record_unpack(bad, sizeof(bad), &out));
    memcpy(bad, r, sizeof(r));
    bad[5] = SETUP_CODE_SOURCE_TEST; // the fallback is never saved
    CHECK(!setup_code_record_unpack(bad, sizeof(bad), &out));
    memcpy(bad, r, sizeof(r));
    bad[7] = 0x10; // discriminator over 12 bits
    CHECK(!setup_code_record_unpack(bad, sizeof(bad), &out));
    setup_code_t forbidden = {.passcode = 11111111u, .discriminator = 1, .source = SETUP_CODE_SOURCE_GENERATED};
    setup_code_record_pack(&forbidden, bad);
    CHECK(!setup_code_record_unpack(bad, sizeof(bad), &out));
}

static int print_vectors(int n)
{
    uint32_t state = 0xC0FFEEu;
    for (int i = 0; i < n; i++) {
        setup_code_t c;
        setup_code_generate(xorshift, &state, &c);
        char m[SETUP_CODE_MANUAL_LEN];
        setup_code_manual_encode(c.passcode, c.discriminator, m);
        printf("%u %u %s\n", (unsigned)c.passcode, (unsigned)c.discriminator, m);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "vectors") == 0) {
        return print_vectors(atoi(argv[2]));
    }
    test_passcode_rules();
    test_known_vector();
    test_parse_rejects();
    test_round_trip();
    test_generate_rejects();
    test_record();
    printf("%d setup code checks, %d failures\n", s_checks, s_failures);
    return s_failures == 0 ? 0 : 1;
}
