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

// test_diag.c
//
// Host tests for the diagnostics log ring (diag_ring.c). Build and run with
// test_host/run.sh.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diag_ring.h"

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

typedef struct {
    char buf[DIAG_RING_SIZE * 2];
    size_t len;
    int spans;
} sink_t;

static void collect(const char *bytes, size_t n, void *ctx)
{
    sink_t *s = (sink_t *)ctx;
    memcpy(s->buf + s->len, bytes, n);
    s->len += n;
    s->spans++;
}

static size_t read_all(const diag_ring_t *r, sink_t *s)
{
    memset(s, 0, sizeof(*s));
    return diag_ring_read(r, collect, s);
}

static diag_ring_t r;

int main(void)
{
    sink_t s;

    // Garbage after power-on resets.
    memset(&r, 0xA5, sizeof(r));
    diag_ring_init(&r, true);
    CHECK(read_all(&r, &s) == 0 && s.spans == 0);
    CHECK(r.boots == 1 && !r.frozen);

    // Plain append and read.
    diag_ring_append(&r, "hello ", 6);
    diag_ring_append(&r, "world\n", 6);
    CHECK(read_all(&r, &s) == 12 && memcmp(s.buf, "hello world\n", 12) == 0 && s.spans == 1);

    // Wrap: the oldest bytes go, the order stays.
    diag_ring_reset(&r);
    char big[DIAG_RING_SIZE + 100];
    for (size_t i = 0; i < sizeof(big); i++) {
        big[i] = (char)('a' + (i % 26));
    }
    diag_ring_append(&r, big, 200);
    diag_ring_append(&r, big + 200, DIAG_RING_SIZE - 200 + 100);
    CHECK(read_all(&r, &s) == DIAG_RING_SIZE);
    CHECK(memcmp(s.buf, big + 100, DIAG_RING_SIZE) == 0);
    CHECK(s.spans == 2);

    // Exactly full, no wrap yet in content order.
    diag_ring_reset(&r);
    diag_ring_append(&r, big, DIAG_RING_SIZE);
    CHECK(read_all(&r, &s) == DIAG_RING_SIZE && memcmp(s.buf, big, DIAG_RING_SIZE) == 0);

    // One append longer than the ring keeps its tail.
    diag_ring_reset(&r);
    diag_ring_append(&r, big, sizeof(big));
    CHECK(read_all(&r, &s) == DIAG_RING_SIZE && memcmp(s.buf, big + 100, DIAG_RING_SIZE) == 0);

    // Frozen: no writes, first reason kept, survives init; unfrozen is reset
    // unless kept.
    diag_ring_reset(&r);
    diag_ring_append(&r, "before\n", 7);
    diag_ring_freeze(&r, "commissioning failed");
    diag_ring_freeze(&r, "second");
    diag_ring_append(&r, "after\n", 6);
    CHECK(read_all(&r, &s) == 7 && memcmp(s.buf, "before\n", 7) == 0);
    CHECK(strcmp(r.reason, "commissioning failed") == 0);
    diag_ring_init(&r, false);
    CHECK(r.frozen && read_all(&r, &s) == 7 && r.boots == 1);
    diag_ring_reset(&r);
    diag_ring_append(&r, "x", 1);
    diag_ring_init(&r, false);
    CHECK(!r.frozen && read_all(&r, &s) == 0);
    diag_ring_append(&r, "y", 1);
    diag_ring_init(&r, true);
    CHECK(read_all(&r, &s) == 1 && r.boots == 2);

    // A long reason is truncated, NUL-terminated.
    diag_ring_reset(&r);
    diag_ring_freeze(&r, "0123456789012345678901234567890123456789");
    CHECK(strlen(r.reason) == sizeof(r.reason) - 1);

    // Corrupt fields reset.
    diag_ring_reset(&r);
    diag_ring_append(&r, "z", 1);
    r.head = DIAG_RING_SIZE;
    diag_ring_init(&r, true);
    CHECK(read_all(&r, &s) == 0);
    diag_ring_reset(&r);
    memset(r.reason, 'q', sizeof(r.reason));
    diag_ring_init(&r, true);
    CHECK(r.reason[0] == '\0');

    printf("%d diagnostics ring checks, %d failures\n", s_checks, s_failures);
    return s_failures == 0 ? 0 : 1;
}
