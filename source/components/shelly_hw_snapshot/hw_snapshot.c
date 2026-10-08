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


// hw_snapshot.c

#include "hw_snapshot.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *const ADE_STATE_NAMES[HW_ADE_STATE_COUNT] = {
    [HW_ADE_NOT_RUN] = "not run",
    [HW_ADE_RUNNING] = "running",
    [HW_ADE_NO_ANSWER] = "no answer",
    [HW_ADE_BUS_ERROR] = "bus error",
    [HW_ADE_READ_ERRORS] = "read errors",
    [HW_ADE_DONE] = "done",
    [HW_ADE_SKIPPED_CRASH] = "skipped after a crash",
};

const char *hw_ade_state_name(uint8_t state)
{
    return state < HW_ADE_STATE_COUNT ? ADE_STATE_NAMES[state] : "unknown";
}

// FNV-1a over every byte before the check.
static uint32_t snap_hash(const hw_snap_t *s)
{
    const uint8_t *p = (const uint8_t *)s;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < offsetof(hw_snap_t, check); i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

void hw_snap_reset(hw_snap_t *s)
{
    memset(s, 0, sizeof(*s));
    s->magic = HW_SNAP_MAGIC;
    s->version = HW_SNAP_VERSION;
    s->ntc_raw = -1;
    s->ntc_mv = -1;
    s->ntc_c10 = HW_SNAP_NO_TEMP;
    s->die_c10 = HW_SNAP_NO_TEMP;
    hw_snap_seal(s);
}

void hw_snap_seal(hw_snap_t *s)
{
    s->check = snap_hash(s);
}

bool hw_snap_valid(const hw_snap_t *s)
{
    return s->magic == HW_SNAP_MAGIC && s->version == HW_SNAP_VERSION && s->ade_count <= HW_SNAP_REGS &&
           s->channels <= HW_SNAP_CHANNELS && s->check == snap_hash(s);
}

void hw_snap_set_reg(hw_snap_t *s, uint16_t addr, uint8_t width, bool ok, uint32_t value)
{
    hw_snap_reg_t *r = NULL;
    for (uint8_t i = 0; i < s->ade_count; i++) {
        if (s->ade[i].addr == addr) {
            r = &s->ade[i];
            break;
        }
    }
    if (r == NULL) {
        if (s->ade_count >= HW_SNAP_REGS) {
            return;
        }
        r = &s->ade[s->ade_count++];
        r->addr = addr;
        r->ok = 0;
        r->value = 0;
    }
    r->width = width;
    if (ok) {
        r->ok = 1;
        r->value = value;
    }
}

bool hw_snap_get_reg(const hw_snap_t *s, uint16_t addr, uint32_t *value)
{
    for (uint8_t i = 0; i < s->ade_count && i < HW_SNAP_REGS; i++) {
        if (s->ade[i].addr == addr && s->ade[i].ok) {
            *value = s->ade[i].value;
            return true;
        }
    }
    return false;
}

// Appends to a buffer that may be too small: keeps counting what it needs.
typedef struct {
    char *buf;
    size_t len;
    size_t used;
} out_t;

static void put(out_t *o, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    size_t room = o->used < o->len ? o->len - o->used : 0;
    int n = vsnprintf(room > 0 ? o->buf + o->used : NULL, room, fmt, ap);
    va_end(ap);
    if (n > 0) {
        o->used += (size_t)n;
    }
}

// A temperature in tenths as a JSON number, or null.
static void put_temp(out_t *o, const char *key, int16_t c10)
{
    if (c10 == HW_SNAP_NO_TEMP) {
        put(o, "\"%s\":null", key);
    } else {
        int a = c10 < 0 ? -c10 : c10;
        put(o, "\"%s\":%s%d.%d", key, c10 < 0 ? "-" : "", a / 10, a % 10);
    }
}

static void put_bits(out_t *o, const char *key, uint32_t bits, uint8_t channels)
{
    put(o, "\"%s\":[", key);
    for (uint8_t ch = 0; ch < channels; ch++) {
        put(o, "%s%s", ch ? "," : "", (bits >> ch) & 1u ? "true" : "false");
    }
    put(o, "]");
}

static uint8_t clamp_channels(uint8_t channels)
{
    if (channels == 0) {
        return 1;
    }
    return channels > HW_SNAP_CHANNELS ? HW_SNAP_CHANNELS : channels;
}

size_t hw_snap_json(const hw_snap_t *s, const hw_live_t *live, char *buf, size_t len)
{
    out_t o = {buf, len, 0};
    if (len > 0) {
        buf[0] = '\0';
    }
    put(&o, "{\"live\":");
    if (live == NULL) {
        put(&o, "null");
    } else {
        uint8_t ch = clamp_channels(live->channels);
        put(&o, "{\"uptime_s\":%lu,\"heap_free\":%lu,\"heap_min\":%lu,", (unsigned long)live->uptime_s,
            (unsigned long)live->heap_free, (unsigned long)live->heap_min);
        put_bits(&o, "relay", live->relay_cmd, ch);
        put(&o, ",");
        put_bits(&o, "relay_pin", live->relay_pin, ch);
        put(&o, ",");
        put_bits(&o, "sw_pin", live->sw_raw, ch);
        put(&o, ",\"button_pin\":%u,\"ntc_raw\":%d,\"ntc_mv\":%d,", live->button_raw, live->ntc_raw, live->ntc_mv);
        put_temp(&o, "ntc_c", live->ntc_c10);
        put(&o, ",");
        put_temp(&o, "die_c", live->die_c10);
        put(&o, "}");
    }
    put(&o, ",\"last_boot\":");
    if (s == NULL || !hw_snap_valid(s)) {
        put(&o, "null}");
        return o.used;
    }
    uint8_t ch = clamp_channels(s->channels);
    put(&o, "{\"boots\":%lu,\"reset_reason\":%lu,\"uptime_s\":%lu,\"heap_free\":%lu,\"heap_min\":%lu,"
            "\"heap_big\":%lu,",
        (unsigned long)s->boots, (unsigned long)s->reset_reason, (unsigned long)s->uptime_s,
        (unsigned long)s->heap_free, (unsigned long)s->heap_min, (unsigned long)s->heap_big);
    put_bits(&o, "relay", s->relay_cmd, ch);
    put(&o, ",");
    put_bits(&o, "relay_pin", s->relay_pin, ch);
    put(&o, ",");
    put_bits(&o, "sw_pin", s->sw_raw, ch);
    put(&o, ",");
    put_bits(&o, "sw_settled", s->sw_stable, ch);
    put(&o, ",\"sw_edges\":[");
    for (uint8_t i = 0; i < ch; i++) {
        put(&o, "%s%lu", i ? "," : "", (unsigned long)s->sw_edges[i]);
    }
    put(&o, "],\"button_pin\":%u,\"ntc_raw\":%d,\"ntc_mv\":%d,", s->button_raw, s->ntc_raw, s->ntc_mv);
    put_temp(&o, "ntc_c", s->ntc_c10);
    put(&o, ",");
    put_temp(&o, "die_c", s->die_c10);
    put(&o, ",\"ntc_ok\":%s,\"ntc_status\":%u,\"governing\":\"%s\",\"overtemp\":%s,\"no_sensor\":%s,",
        s->ntc_ok ? "true" : "false", s->ntc_status, s->governing_ntc ? "ntc" : "die", s->overtemp ? "true" : "false",
        s->no_sensor ? "true" : "false");
    put(&o, "\"meter\":{\"state\":\"%s\",\"error\":%ld,\"dump_s\":%lu,\"live_s\":%lu,\"live_reads\":%lu,\"regs\":{",
        hw_ade_state_name(s->ade_state), (long)s->ade_err, (unsigned long)s->ade_dump_s, (unsigned long)s->ade_live_s,
        (unsigned long)s->ade_live_reads);
    bool first = true;
    for (uint8_t i = 0; i < s->ade_count; i++) {
        const hw_snap_reg_t *r = &s->ade[i];
        if (!r->ok) {
            put(&o, "%s\"0x%03X\":null", first ? "" : ",", r->addr);
        } else {
            put(&o, "%s\"0x%03X\":\"0x%0*lX\"", first ? "" : ",", r->addr, (int)(r->width / 4), (unsigned long)r->value);
        }
        first = false;
    }
    put(&o, "}}}}");
    return o.used;
}

static void put_c10(out_t *o, int16_t c10)
{
    if (c10 == HW_SNAP_NO_TEMP) {
        put(o, "-");
        return;
    }
    int a = c10 < 0 ? -c10 : c10;
    put(o, "%s%d.%dC", c10 < 0 ? "-" : "", a / 10, a % 10);
}

static void put_mask(out_t *o, uint32_t bits, uint8_t channels)
{
    for (uint8_t ch = 0; ch < channels; ch++) {
        put(o, "%u", (unsigned)((bits >> ch) & 1u));
    }
}

size_t hw_snap_line(const hw_snap_t *s, char *buf, size_t len)
{
    out_t o = {buf, len, 0};
    if (len > 0) {
        buf[0] = '\0';
    }
    uint8_t ch = clamp_channels(s->channels);
    put(&o, "rel ");
    put_mask(&o, s->relay_cmd, ch);
    put(&o, "/");
    put_mask(&o, s->relay_pin, ch);
    put(&o, " sw ");
    put_mask(&o, s->sw_raw, ch);
    put(&o, "/");
    put_mask(&o, s->sw_stable, ch);
    put(&o, " e ");
    for (uint8_t i = 0; i < ch; i++) {
        put(&o, "%s%lu", i ? "," : "", (unsigned long)(s->sw_edges[i] % 100000u));
    }
    put(&o, " btn %u ntc %dmV ", s->button_raw, s->ntc_mv);
    put_c10(&o, s->ntc_c10);
    put(&o, " die ");
    put_c10(&o, s->die_c10);
    put(&o, " heap %luk", (unsigned long)(s->heap_free / 1024u));
    return o.used < len ? o.used : (len > 0 ? len - 1 : 0);
}
