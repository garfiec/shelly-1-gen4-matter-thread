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

// maintenance_dns.c

#include "maintenance_dns.h"

#include <string.h>

#define DNS_HEADER_LEN  12
#define DNS_TYPE_A      1
#define DNS_CLASS_IN    1
#define DNS_TTL_S       60

size_t dns_captive_reply(const uint8_t *query, size_t query_len, const uint8_t ip[4], uint8_t *out,
                         size_t out_cap)
{
    if (query_len < DNS_HEADER_LEN) {
        return 0;
    }
    // Only standard queries (QR=0, opcode 0) with exactly one question.
    if ((query[2] & 0x80) != 0 || (query[2] & 0x78) != 0) {
        return 0;
    }
    uint16_t qdcount = (uint16_t)((query[4] << 8) | query[5]);
    if (qdcount != 1) {
        return 0;
    }

    // Walk the question name: labels until a zero byte. No compression is
    // valid in a question.
    size_t pos = DNS_HEADER_LEN;
    while (pos < query_len && query[pos] != 0) {
        if ((query[pos] & 0xC0) != 0) {
            return 0;
        }
        pos += (size_t)query[pos] + 1;
    }
    if (pos >= query_len) {
        return 0;
    }
    pos++;  // The zero byte
    if (pos + 4 > query_len) {
        return 0;
    }
    uint16_t qtype = (uint16_t)((query[pos] << 8) | query[pos + 1]);
    uint16_t qclass = (uint16_t)((query[pos + 2] << 8) | query[pos + 3]);
    pos += 4;
    size_t question_end = pos;

    int answer = qtype == DNS_TYPE_A && qclass == DNS_CLASS_IN;
    size_t reply_len = question_end + (answer ? 16 : 0);
    if (reply_len > out_cap) {
        return 0;
    }

    // Header and question, copied; anything after the question (EDNS) is
    // dropped, so ARCOUNT goes to 0.
    memcpy(out, query, question_end);
    out[2] = 0x84 | (query[2] & 0x01);  // QR, AA, keep RD
    out[3] = 0x80;                      // RA, NOERROR
    out[6] = 0;
    out[7] = answer ? 1 : 0;            // ANCOUNT
    out[8] = 0;
    out[9] = 0;                         // NSCOUNT
    out[10] = 0;
    out[11] = 0;                        // ARCOUNT

    if (answer) {
        uint8_t *a = out + question_end;
        a[0] = 0xC0;                    // Name: pointer to the question
        a[1] = DNS_HEADER_LEN;
        a[2] = 0;
        a[3] = DNS_TYPE_A;
        a[4] = 0;
        a[5] = DNS_CLASS_IN;
        a[6] = 0;
        a[7] = 0;
        a[8] = 0;
        a[9] = DNS_TTL_S;
        a[10] = 0;
        a[11] = 4;                      // RDLENGTH
        memcpy(a + 12, ip, 4);
    }
    return reply_len;
}
