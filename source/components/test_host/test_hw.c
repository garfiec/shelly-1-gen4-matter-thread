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


// test_hw.c
//
// Host tests for the board bring-up data: the hardware snapshot's storage
// and text forms (hw_snapshot.c), and the ADE7953 register table the
// snapshot reads (ade7953_regs.c), which must never contain a register
// whose read changes the chip.

#include <stdio.h>
#include <string.h>

#include "ade7953_regs.h"
#include "hw_snapshot.h"

static int s_failures = 0;
static int s_checks = 0;

#define CHECK(cond, ...)                \
    do {                                \
        s_checks++;                     \
        if (!(cond)) {                  \
            s_failures++;               \
            printf("FAIL: ");           \
            printf(__VA_ARGS__);        \
            printf("\n");               \
        }                               \
    } while (0)

// Balanced braces and brackets outside strings, and no stray quotes: enough
// to catch a formatting slip in the hand-built JSON.
static int json_shape_ok(const char *j)
{
    int depth = 0;
    int in_str = 0;
    for (const char *p = j; *p; p++) {
        if (in_str) {
            if (*p == '"') {
                in_str = 0;
            }
            continue;
        }
        if (*p == '"') {
            in_str = 1;
        } else if (*p == '{' || *p == '[') {
            depth++;
        } else if (*p == '}' || *p == ']') {
            if (--depth < 0) {
                return 0;
            }
        }
    }
    return depth == 0 && !in_str;
}

static void test_snapshot(void)
{
    hw_snap_t s;
    memset(&s, 0xA5, sizeof(s));
    CHECK(!hw_snap_valid(&s), "garbage after a power cut is not valid");
    hw_snap_reset(&s);
    CHECK(hw_snap_valid(&s), "a fresh snapshot is valid");
    s.boots = 7;
    CHECK(!hw_snap_valid(&s), "a change without a seal is caught");
    hw_snap_seal(&s);
    CHECK(hw_snap_valid(&s), "sealed again");
    s.ade_count = HW_SNAP_REGS + 1;
    hw_snap_seal(&s);
    CHECK(!hw_snap_valid(&s), "an out-of-range register count is refused");
    hw_snap_reset(&s);

    // Registers: add, update, keep the old value on a failed read, cap.
    uint32_t v = 0;
    hw_snap_set_reg(&s, 0x31C, 32, true, 0x002F6A1B);
    CHECK(hw_snap_get_reg(&s, 0x31C, &v) && v == 0x002F6A1B, "stored VRMS");
    hw_snap_set_reg(&s, 0x31C, 32, false, 0);
    CHECK(hw_snap_get_reg(&s, 0x31C, &v) && v == 0x002F6A1B, "a failed read keeps the value");
    hw_snap_set_reg(&s, 0x702, 8, false, 0);
    CHECK(!hw_snap_get_reg(&s, 0x702, &v), "never read is absent");
    for (int i = 0; i < HW_SNAP_REGS + 5; i++) {
        hw_snap_set_reg(&s, (uint16_t)(0x380 + i), 32, true, (uint32_t)i);
    }
    CHECK(s.ade_count == HW_SNAP_REGS, "the table stops at its size (%u)", s.ade_count);

    // JSON: a full snapshot and live values, measured then written.
    hw_snap_reset(&s);
    s.channels = 2;
    s.boots = 3;
    s.relay_cmd = 2;
    s.relay_pin = 2;
    s.sw_raw = 1;
    s.sw_edges[0] = 4;
    s.sw_edges[1] = 9;
    s.ntc_mv = 1820;
    s.ntc_c10 = 412;
    s.die_c10 = -5;
    s.ade_state = HW_ADE_DONE;
    hw_snap_set_reg(&s, 0x702, 8, true, 0x0A);
    hw_snap_set_reg(&s, 0x10E, 16, true, 0x0E90);
    hw_snap_set_reg(&s, 0x31C, 32, true, 0x002F6A1B);
    hw_snap_set_reg(&s, 0x31A, 32, false, 0);
    hw_snap_seal(&s);
    hw_live_t live = {.uptime_s = 12, .channels = 2, .relay_cmd = 1, .relay_pin = 1, .sw_raw = 2, .button_raw = 1,
                      .ntc_raw = 2100, .ntc_mv = 1700, .ntc_c10 = HW_SNAP_NO_TEMP, .die_c10 = 455};
    size_t need = hw_snap_json(&s, &live, NULL, 0);
    char buf[4096];
    CHECK(need < sizeof(buf), "JSON fits the test buffer (%zu)", need);
    size_t got = hw_snap_json(&s, &live, buf, sizeof(buf));
    CHECK(got == need && strlen(buf) == need, "measured and written lengths agree");
    CHECK(json_shape_ok(buf), "JSON is balanced: %s", buf);
    CHECK(strstr(buf, "\"relay\":[true,false]") != NULL, "live relays");
    CHECK(strstr(buf, "\"relay\":[false,true]") != NULL, "last boot relays");
    CHECK(strstr(buf, "\"sw_edges\":[4,9]") != NULL, "edges");
    CHECK(strstr(buf, "\"ntc_c\":41.2") != NULL && strstr(buf, "\"die_c\":-0.5") != NULL, "temperatures");
    CHECK(strstr(buf, "\"ntc_c\":null") != NULL, "unknown temperature is null");
    CHECK(strstr(buf, "\"0x702\":\"0x0A\"") != NULL, "8-bit register as 2 digits");
    CHECK(strstr(buf, "\"0x10E\":\"0x0E90\"") != NULL, "16-bit register as 4 digits");
    CHECK(strstr(buf, "\"0x31C\":\"0x002F6A1B\"") != NULL, "32-bit register as 8 digits");
    CHECK(strstr(buf, "\"0x31A\":null") != NULL, "an unread register is null");
    CHECK(strstr(buf, "\"state\":\"done\"") != NULL, "probe state");

    // A short buffer is never overrun and still NUL-terminated.
    char small[40];
    memset(small, 'x', sizeof(small));
    size_t n = hw_snap_json(&s, &live, small, sizeof(small));
    CHECK(n == need && small[sizeof(small) - 1] == '\0', "truncated safely");

    // No valid last boot, no live values.
    hw_snap_t bad;
    memset(&bad, 0, sizeof(bad));
    got = hw_snap_json(&bad, NULL, buf, sizeof(buf));
    CHECK(strcmp(buf, "{\"live\":null,\"last_boot\":null}") == 0, "empty form: %s", buf);

    // The log line stays within the diagnostics log's 100-byte lines, with
    // the widest values.
    hw_snap_reset(&s);
    s.channels = 2;
    s.relay_cmd = s.relay_pin = 3;
    s.sw_raw = s.sw_stable = 3;
    s.sw_edges[0] = s.sw_edges[1] = 4000000000u;
    s.ntc_mv = -1;
    s.ntc_c10 = -2730;
    s.die_c10 = -2730;
    s.heap_free = 0xFFFFFFFFu;
    char line[96];
    size_t ll = hw_snap_line(&s, line, sizeof(line));
    CHECK(ll == strlen(line) && ll + 16 <= 100, "line fits with the log prefix (%zu): %s", ll, line);
    hw_snap_reset(&s);
    s.channels = 2;
    s.relay_cmd = 1;
    s.ntc_mv = 1820;
    s.ntc_c10 = 412;
    s.die_c10 = 451;
    s.heap_free = 172 * 1024;
    hw_snap_line(&s, line, sizeof(line));
    CHECK(strcmp(line, "rel 10/00 sw 00/00 e 0,0 btn 0 ntc 1820mV 41.2C die 45.1C heap 172k") == 0, "line: %s", line);

    CHECK(strcmp(hw_ade_state_name(HW_ADE_SKIPPED_CRASH), "skipped after a crash") == 0, "state names");
    CHECK(strcmp(hw_ade_state_name(200), "unknown") == 0, "unknown state");
}

static void test_ade_table(void)
{
    CHECK(ade7953_reg_width(0x702) == 8 && ade7953_reg_width(0x800) == 8, "VERSION and EX_REF are 8-bit");
    CHECK(ade7953_reg_width(0x007) == 8 && ade7953_reg_width(0x102) == 16 && ade7953_reg_width(0x21C) == 24 &&
              ade7953_reg_width(0x31C) == 32,
          "widths by address range");
    CHECK(ade7953_reg_width(0x500) == 0, "outside the map");

    // Every reset-on-read register, at both addresses.
    static const uint16_t RESETS[] = {0x31E, 0x31F, 0x320, 0x321, 0x322, 0x323, 0x327, 0x329, 0x32B, 0x32E, 0x331,
                                      0x21E, 0x21F, 0x220, 0x221, 0x222, 0x223, 0x227, 0x229, 0x22B, 0x22E, 0x231};
    for (size_t i = 0; i < sizeof(RESETS) / sizeof(RESETS[0]); i++) {
        CHECK(ade7953_reg_read_resets(RESETS[i]), "0x%03X must count as reset-on-read", RESETS[i]);
    }
    CHECK(!ade7953_reg_read_resets(0x31C) && !ade7953_reg_read_resets(0x32D) && !ade7953_reg_read_resets(0x102),
          "plain registers are readable");

    int live = 0;
    for (int i = 0; i < ADE7953_PROBE_REG_COUNT; i++) {
        uint16_t a = ADE7953_PROBE_REGS[i].addr;
        CHECK(!ade7953_reg_read_resets(a), "%s (0x%03X) changes the chip when read", ADE7953_PROBE_REGS[i].name, a);
        CHECK(ade7953_reg_width(a) != 0, "%s (0x%03X) has no width", ADE7953_PROBE_REGS[i].name, a);
        CHECK(a != 0x0FE, "the unlock register is never touched");
        for (int j = i + 1; j < ADE7953_PROBE_REG_COUNT; j++) {
            CHECK(ADE7953_PROBE_REGS[j].addr != a, "0x%03X listed twice", a);
        }
        live += ADE7953_PROBE_REGS[i].live;
    }
    CHECK(ADE7953_PROBE_REG_COUNT <= HW_SNAP_REGS, "the snapshot holds every register (%d)", ADE7953_PROBE_REG_COUNT);
    // The measurements the metering build needs are in the live subset.
    static const uint16_t NEEDED[] = {0x31C, 0x31A, 0x31B, 0x312, 0x313, 0x10E};
    for (size_t k = 0; k < sizeof(NEEDED) / sizeof(NEEDED[0]); k++) {
        int found = 0;
        for (int i = 0; i < ADE7953_PROBE_REG_COUNT; i++) {
            found |= ADE7953_PROBE_REGS[i].addr == NEEDED[k] && ADE7953_PROBE_REGS[i].live;
        }
        CHECK(found, "0x%03X is read every minute", NEEDED[k]);
    }
    CHECK(live > 0, "a live subset exists");
}

int main(void)
{
    test_snapshot();
    test_ade_table();
    if (s_failures) {
        printf("%d of %d hardware data checks FAILED\n", s_failures, s_checks);
        return 1;
    }
    printf("All %d hardware data checks passed\n", s_checks);
    return 0;
}
