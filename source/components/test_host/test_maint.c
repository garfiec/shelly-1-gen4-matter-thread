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


// test_maint.c
//
// Host tests for Wi-Fi maintenance mode's pure logic: the upload check
// (maintenance_image) and the captive-portal DNS reply (maintenance_dns).
// Build and run with test_host/run.sh.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "maintenance_dns.h"
#include "maintenance_image.h"
#include "qr_svg.h"

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

#define TLV_LEN     40
#define APP_LEN     2048

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

// A Matter .ota wrapping a minimal ESP32-C6 app image.
static size_t make_ota(uint8_t *buf, const char *hw, const char *version, const char *project, uint16_t chip)
{
    size_t total = MI_OTA_FIXED_HEADER + TLV_LEN + APP_LEN;
    memset(buf, 0, total);
    put32(buf, MI_OTA_MAGIC);
    put32(buf + 4, (uint32_t)total);
    put32(buf + 8, 0);
    put32(buf + 12, TLV_LEN);
    memset(buf + MI_OTA_FIXED_HEADER, 0x15, TLV_LEN);
    uint8_t *img = buf + MI_OTA_FIXED_HEADER + TLV_LEN;
    img[0] = MI_IMAGE_MAGIC;
    img[12] = chip & 0xFF;
    img[13] = chip >> 8;
    put32(img + MI_APP_DESC_OFFSET, MI_APP_DESC_MAGIC);
    strcpy((char *)img + MI_APP_DESC_OFFSET + 16, version);
    strcpy((char *)img + MI_APP_DESC_OFFSET + 48, project);
    if (hw != NULL) {
        put32(img + MI_HW_DESC_OFFSET, SHELLY_HW_DESC_MAGIC);
        strcpy((char *)img + MI_HW_DESC_OFFSET + 4, hw);
    }
    return total;
}

static void test_image(void)
{
    static uint8_t buf[MI_OTA_FIXED_HEADER + TLV_LEN + APP_LEN];
    mi_info_t info;

    size_t total = make_ota(buf, "S1PMG4", "1.2.3", "outlet", MI_CHIP_ID_ESP32C6);
    CHECK(mi_check(buf, total, "S1PMG4", &info) == MI_OK);
    CHECK(info.total_size == total);
    CHECK(info.payload_offset == MI_OTA_FIXED_HEADER + TLV_LEN);
    CHECK(info.payload_size == APP_LEN);
    CHECK(strcmp(info.version, "1.2.3") == 0);
    CHECK(strcmp(info.project, "outlet") == 0);
    CHECK(strcmp(info.hw_code, "S1PMG4") == 0);

    // Fed a little at a time, it asks for more until it can decide.
    size_t have = 0;
    mi_result_t r;
    int rounds = 0;
    while ((r = mi_check(buf, have, "S1PMG4", &info)) == MI_NEED_MORE && rounds++ < 10) {
        CHECK(info.need_bytes > have);
        have = info.need_bytes;
    }
    CHECK(r == MI_OK);
    CHECK(have == MI_OTA_FIXED_HEADER + TLV_LEN + MI_HW_DESC_OFFSET + sizeof(shelly_hw_desc_t));

    // Wrong hardware, missing hardware code, wrong chip.
    CHECK(mi_check(buf, total, "S1G4", &info) == MI_WRONG_HARDWARE);
    CHECK(mi_check(buf, total, NULL, &info) == MI_WRONG_HARDWARE);
    make_ota(buf, "S1PMG4X", "1.2.3", "outlet", MI_CHIP_ID_ESP32C6);
    CHECK(mi_check(buf, total, "S1PMG4", &info) == MI_WRONG_HARDWARE);  // Not a prefix match
    make_ota(buf, NULL, "1.2.3", "outlet", MI_CHIP_ID_ESP32C6);
    CHECK(mi_check(buf, total, "S1PMG4", &info) == MI_NO_HW_CODE);
    make_ota(buf, "S1PMG4", "1.2.3", "outlet", 0x0005);  // ESP32-C3
    CHECK(mi_check(buf, total, "S1PMG4", &info) == MI_WRONG_CHIP);

    // A raw app .bin or a merged .bin: starts with the image magic.
    uint8_t *img = buf + MI_OTA_FIXED_HEADER + TLV_LEN;
    make_ota(buf, "S1PMG4", "1.2.3", "outlet", MI_CHIP_ID_ESP32C6);
    CHECK(mi_check(img, APP_LEN, "S1PMG4", &info) == MI_NOT_OTA_FILE);

    // Not a firmware file at all.
    static const uint8_t junk[] = "<html>not firmware</html>";
    CHECK(mi_check(junk, sizeof(junk), "S1PMG4", &info) == MI_BAD_HEADER);
    CHECK(mi_check(junk, 2, "S1PMG4", &info) == MI_NEED_MORE);

    // Corrupt headers.
    make_ota(buf, "S1PMG4", "1.2.3", "outlet", MI_CHIP_ID_ESP32C6);
    put32(buf + 12, 0);
    CHECK(mi_check(buf, total, "S1PMG4", &info) == MI_BAD_HEADER);
    put32(buf + 12, MI_OTA_MAX_TLV + 1);
    CHECK(mi_check(buf, total, "S1PMG4", &info) == MI_BAD_HEADER);
    put32(buf + 12, TLV_LEN);
    put32(buf + 4, 100);  // Total smaller than the header plus an app description
    CHECK(mi_check(buf, total, "S1PMG4", &info) == MI_BAD_HEADER);
    make_ota(buf, "S1PMG4", "1.2.3", "outlet", MI_CHIP_ID_ESP32C6);
    img[0] = 0;
    CHECK(mi_check(buf, total, "S1PMG4", &info) == MI_NOT_AN_APP);
    make_ota(buf, "S1PMG4", "1.2.3", "outlet", MI_CHIP_ID_ESP32C6);
    put32(img + MI_APP_DESC_OFFSET, 0);
    CHECK(mi_check(buf, total, "S1PMG4", &info) == MI_NOT_AN_APP);

    // A version string that fills its whole field stays terminated.
    make_ota(buf, "S1PMG4", "1.2.3", "outlet", MI_CHIP_ID_ESP32C6);
    memset(img + MI_APP_DESC_OFFSET + 16, '9', 32);
    CHECK(mi_check(buf, total, "S1PMG4", &info) == MI_OK);
    CHECK(strlen(info.version) == 32);

    // Every result has user-facing text.
    for (int i = MI_NEED_MORE; i <= MI_WRONG_HARDWARE; i++) {
        CHECK(mi_result_text((mi_result_t)i)[0] != '\0');
    }

    // Versions.
    CHECK(mi_version_cmp("1.0.0", "1.0.0") == 0);
    CHECK(mi_version_cmp("1.0.1", "1.0.0") > 0);
    CHECK(mi_version_cmp("1.0.0", "1.1.0") < 0);
    CHECK(mi_version_cmp("2.0.0", "1.9.9") > 0);
    CHECK(mi_version_cmp("1.10.0", "1.9.0") > 0);
    CHECK(mi_version_cmp("1.0", "1.0.0") == 0);
    CHECK(mi_version_cmp("1.2.3-dirty", "1.2.3") == 0);
    CHECK(mi_version_cmp("", "0.0.0") == 0);
}

// ---------------------------------------------------------------------------
// Captive-portal DNS
// ---------------------------------------------------------------------------

static size_t make_query(uint8_t *q, const char *name, uint16_t qtype)
{
    size_t n = 0;
    q[n++] = 0x12;  // ID
    q[n++] = 0x34;
    q[n++] = 0x01;  // RD
    q[n++] = 0x00;
    q[n++] = 0;
    q[n++] = 1;     // QDCOUNT
    for (int i = 0; i < 6; i++) {
        q[n++] = 0;
    }
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        q[n++] = (uint8_t)len;
        memcpy(q + n, p, len);
        n += len;
        p += len + (dot ? 1 : 0);
    }
    q[n++] = 0;
    q[n++] = qtype >> 8;
    q[n++] = qtype & 0xFF;
    q[n++] = 0;
    q[n++] = 1;     // IN
    return n;
}

static void test_dns(void)
{
    static const uint8_t ip[4] = {192, 168, 33, 1};
    uint8_t q[256], r[300];

    size_t qn = make_query(q, "connectivitycheck.gstatic.com", 1);
    size_t rn = dns_captive_reply(q, qn, ip, r, sizeof(r));
    CHECK(rn == qn + 16);
    CHECK(r[0] == 0x12 && r[1] == 0x34);           // Same ID
    CHECK((r[2] & 0x80) != 0);                      // A response
    CHECK((r[2] & 0x01) != 0);                      // RD kept
    CHECK((r[3] & 0x0F) == 0);                      // NOERROR
    CHECK(r[7] == 1);                               // One answer
    CHECK(memcmp(r + qn + 12, ip, 4) == 0);         // Pointing here
    CHECK(r[qn] == 0xC0 && r[qn + 1] == 12);        // Name compressed to the question

    // AAAA: an empty answer, so phones fall back to IPv4.
    qn = make_query(q, "captive.apple.com", 28);
    rn = dns_captive_reply(q, qn, ip, r, sizeof(r));
    CHECK(rn == qn);
    CHECK(r[7] == 0);

    // EDNS additional record after the question is dropped.
    qn = make_query(q, "example.com", 1);
    q[11] = 1;  // ARCOUNT
    memset(q + qn, 0, 11);
    rn = dns_captive_reply(q, qn + 11, ip, r, sizeof(r));
    CHECK(rn == qn + 16);
    CHECK(r[11] == 0);

    // Not answered: responses, other opcodes, several questions, truncation,
    // a name that runs off the end, compression in the question, no room.
    qn = make_query(q, "example.com", 1);
    q[2] |= 0x80;
    CHECK(dns_captive_reply(q, qn, ip, r, sizeof(r)) == 0);
    qn = make_query(q, "example.com", 1);
    q[2] |= 0x10;  // Opcode 2
    CHECK(dns_captive_reply(q, qn, ip, r, sizeof(r)) == 0);
    qn = make_query(q, "example.com", 1);
    q[5] = 2;
    CHECK(dns_captive_reply(q, qn, ip, r, sizeof(r)) == 0);
    CHECK(dns_captive_reply(q, 5, ip, r, sizeof(r)) == 0);
    qn = make_query(q, "example.com", 1);
    CHECK(dns_captive_reply(q, qn - 3, ip, r, sizeof(r)) == 0);
    CHECK(dns_captive_reply(q, 14, ip, r, sizeof(r)) == 0);
    q[12] = 0xC0;
    CHECK(dns_captive_reply(q, qn, ip, r, sizeof(r)) == 0);
    qn = make_query(q, "example.com", 1);
    CHECK(dns_captive_reply(q, qn, ip, r, qn + 15) == 0);
}

typedef struct {
    char buf[8192];
    size_t len;
    int pieces;
} svg_sink_t;

static void svg_emit(const char *text, size_t len, void *ctx)
{
    svg_sink_t *s = (svg_sink_t *)ctx;
    if (s->len + len < sizeof(s->buf)) {
        memcpy(s->buf + s->len, text, len);
        s->len += len;
        s->buf[s->len] = '\0';
    }
    s->pieces++;
}

// Count the dark modules the path draws: each "h<run>" adds run.
static int svg_dark_modules(const char *svg)
{
    int dark = 0;
    for (const char *p = strstr(svg, " d=\""); p != NULL && (p = strchr(p, 'h')) != NULL; p++) {
        if (p[1] != '-') {
            dark += atoi(p + 1);
        }
    }
    return dark;
}

static void test_qr_svg(void)
{
    static svg_sink_t s;
    memset(&s, 0, sizeof(s));
    // A made-up code's QR text: version 1 (21 modules) plus the quiet zone.
    CHECK(qr_svg_write("MT:-24J042C00KA0648G00", svg_emit, &s));
    static const char head[] = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 29 29\"";
    CHECK(strncmp(s.buf, head, sizeof(head) - 1) == 0);
    CHECK(s.len > 9 && strcmp(s.buf + s.len - 9, "\"/></svg>") == 0);
    CHECK(strstr(s.buf, "<rect width=\"29\" height=\"29\" fill=\"#fff\"/>") != NULL);
    // The three finder patterns alone are 3 x 33 dark modules; every module
    // is inside the symbol (4 to 24).
    int dark = svg_dark_modules(s.buf);
    CHECK(dark > 99 && dark < 21 * 21);
    CHECK(strstr(s.buf, "M4 4h7v1h-7z") != NULL); // The top-left finder's top row
    CHECK(strstr(s.buf, "M3 ") == NULL && strstr(s.buf, "M25 ") == NULL);
    // The same text gives the same image.
    static svg_sink_t again;
    memset(&again, 0, sizeof(again));
    CHECK(qr_svg_write("MT:-24J042C00KA0648G00", svg_emit, &again) && strcmp(again.buf, s.buf) == 0);
    // Longer text takes a larger version; too long is refused.
    memset(&s, 0, sizeof(s));
    CHECK(qr_svg_write("MT:-24J042C00KA0648G00MT:-24J042C00KA0648G00", svg_emit, &s));
    CHECK(strstr(s.buf, "viewBox=\"0 0 29 29\"") == NULL);
    char big[400];
    memset(big, 'A', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    memset(&s, 0, sizeof(s));
    CHECK(!qr_svg_write(big, svg_emit, &s) && s.pieces == 0);
}

int main(void)
{
    test_image();
    test_dns();
    test_qr_svg();
    printf("%d maintenance checks, %d failures\n", s_checks, s_failures);
    return s_failures == 0 ? 0 : 1;
}
