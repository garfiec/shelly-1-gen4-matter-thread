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

// diag_ring.h
//
// The diagnostics log's storage: a fixed-size ring of text in memory that
// survives a software restart (diag_log.cpp, CONFIG_SHELLY_DIAG_LOG). Pure
// C, no ESP-IDF includes, so the host tests cover it. Not thread-safe: the
// caller serializes.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DIAG_RING_SIZE 6144

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t frozen;       // No more writes until cleared
    uint8_t wrapped;      // The data area has been filled at least once
    uint32_t head;        // Next write position, < DIAG_RING_SIZE
    uint32_t boots;       // Normal boots since the ring was last reset
    char reason[32];      // Why it froze, NUL-terminated
    char data[DIAG_RING_SIZE];
} diag_ring_t;

// Validate the ring after a restart. A bad header (power-on garbage, a
// different build) resets it. keep_unfrozen false starts an unfrozen ring
// over; a frozen ring is always kept. Counts a boot.
void diag_ring_init(diag_ring_t *r, bool keep_unfrozen);

// Whether the header is intact (read-only check, for the maintenance boot).
bool diag_ring_valid(const diag_ring_t *r);

// Empty it and unfreeze it.
void diag_ring_reset(diag_ring_t *r);

// Append n bytes, overwriting the oldest. Nothing while frozen.
void diag_ring_append(diag_ring_t *r, const char *bytes, size_t n);

// Stop writing and record why (truncated to fit). Freezing twice keeps the
// first reason.
void diag_ring_freeze(diag_ring_t *r, const char *reason);

// The content, oldest first, as at most two spans. Returns the total length.
typedef void (*diag_ring_span_fn)(const char *bytes, size_t n, void *ctx);
size_t diag_ring_read(const diag_ring_t *r, diag_ring_span_fn fn, void *ctx);

#ifdef __cplusplus
}
#endif
