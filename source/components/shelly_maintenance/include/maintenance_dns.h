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

// maintenance_dns.h
//
// The captive-portal DNS answer: every name resolves to the device, so a
// phone that joins the maintenance network opens the device's page by
// itself. Pure C, no ESP-IDF includes, for the host tests.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Build the reply to one DNS query. An A query for any name gets ip; any
// other query type gets an empty NOERROR answer. Returns the reply length,
// or 0 if the packet is not a single-question query to answer (then send
// nothing).
size_t dns_captive_reply(const uint8_t *query, size_t query_len, const uint8_t ip[4], uint8_t *out,
                         size_t out_cap);

#ifdef __cplusplus
}
#endif
