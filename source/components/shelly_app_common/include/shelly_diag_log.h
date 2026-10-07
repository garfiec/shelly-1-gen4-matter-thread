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

// shelly_diag_log.h
//
// Optional diagnostics log (CONFIG_SHELLY_DIAG_LOG, off by default): the
// last 6 KB of the log in memory that survives a software restart, shown on
// the Wi-Fi maintenance page. Without the option every function is an empty
// inline, so callers need no #if.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sdkconfig.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*shelly_diag_span_fn)(const char *bytes, size_t n, void *ctx);

#if CONFIG_SHELLY_DIAG_LOG

// Normal boot only (shelly_ui_init calls it): start capturing.
void shelly_diag_log_start(void);

// Once a second from the health task: freeze deadline, status lines.
void shelly_diag_log_tick(void);

// A marker line for a Matter device event (int: the DeviceEventType).
void shelly_diag_log_event(int type);

// For the maintenance page. available: the ring holds a valid log.
bool shelly_diag_log_available(void);
// The header text into buf, then the content oldest first through fn.
size_t shelly_diag_log_header(char *buf, size_t len);
void shelly_diag_log_read(shelly_diag_span_fn fn, void *ctx);
void shelly_diag_log_clear(void);

#else

static inline void shelly_diag_log_start(void) {}
static inline void shelly_diag_log_tick(void) {}
static inline void shelly_diag_log_event(int type) { (void)type; }
static inline bool shelly_diag_log_available(void) { return false; }

#endif

#ifdef __cplusplus
}
#endif
