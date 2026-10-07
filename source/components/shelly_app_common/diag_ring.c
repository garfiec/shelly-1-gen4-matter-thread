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

// diag_ring.c

#include "diag_ring.h"

#include <string.h>

#define DIAG_MAGIC   0x474C4453u // 'SDLG'
#define DIAG_VERSION 1u

void diag_ring_reset(diag_ring_t *r)
{
    r->magic = DIAG_MAGIC;
    r->version = DIAG_VERSION;
    r->frozen = 0;
    r->wrapped = 0;
    r->head = 0;
    r->boots = 0;
    r->reason[0] = '\0';
}

bool diag_ring_valid(const diag_ring_t *r)
{
    return r->magic == DIAG_MAGIC && r->version == DIAG_VERSION && r->head < DIAG_RING_SIZE && r->frozen <= 1 &&
           r->wrapped <= 1 && memchr(r->reason, '\0', sizeof(r->reason)) != NULL;
}

void diag_ring_init(diag_ring_t *r, bool keep_unfrozen)
{
    if (!diag_ring_valid(r) || (!r->frozen && !keep_unfrozen)) {
        diag_ring_reset(r);
    }
    r->boots++;
}

void diag_ring_append(diag_ring_t *r, const char *bytes, size_t n)
{
    if (r->frozen || n == 0) {
        return;
    }
    if (n >= DIAG_RING_SIZE) {
        // Only the last DIAG_RING_SIZE bytes can survive.
        bytes += n - DIAG_RING_SIZE;
        n = DIAG_RING_SIZE;
    }
    size_t first = DIAG_RING_SIZE - r->head;
    if (n < first) {
        memcpy(r->data + r->head, bytes, n);
        r->head += (uint32_t)n;
        return;
    }
    memcpy(r->data + r->head, bytes, first);
    memcpy(r->data, bytes + first, n - first);
    r->head = (uint32_t)(n - first);
    r->wrapped = 1;
}

void diag_ring_freeze(diag_ring_t *r, const char *reason)
{
    if (r->frozen) {
        return;
    }
    r->frozen = 1;
    strncpy(r->reason, reason != NULL ? reason : "", sizeof(r->reason) - 1);
    r->reason[sizeof(r->reason) - 1] = '\0';
}

size_t diag_ring_read(const diag_ring_t *r, diag_ring_span_fn fn, void *ctx)
{
    if (!r->wrapped) {
        if (r->head > 0) {
            fn(r->data, r->head, ctx);
        }
        return r->head;
    }
    fn(r->data + r->head, DIAG_RING_SIZE - r->head, ctx);
    if (r->head > 0) {
        fn(r->data, r->head, ctx);
    }
    return DIAG_RING_SIZE;
}
