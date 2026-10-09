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


// calibration_logic.c

#include "calibration_logic.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool in_bounds(float r)
{
    return isfinite(r) && r >= CAL_RATIO_MIN && r <= CAL_RATIO_MAX;
}

static bool given(float v)
{
    return !isnan(v);
}

void cal_ratios_default(cal_ratios_t *r)
{
    for (int i = 0; i < CAL_Q_COUNT; i++) {
        r->q[i] = 1.0f;
    }
}

bool cal_ratios_valid(const cal_ratios_t *r)
{
    for (int i = 0; i < CAL_Q_COUNT; i++) {
        if (!in_bounds(r->q[i])) {
            return false;
        }
    }
    return true;
}

bool cal_ratios_is_default(const cal_ratios_t *r)
{
    for (int i = 0; i < CAL_Q_COUNT; i++) {
        if (r->q[i] != 1.0f) {
            return false;
        }
    }
    return true;
}

const char *cal_result_message(cal_result_t r)
{
    switch (r) {
    case CAL_OK:
        return "";
    case CAL_ERR_NOTHING:
        return "Enter at least one value from your meter";
    case CAL_ERR_INPUT:
        return "Enter each value as a positive number, like 117.4";
    case CAL_ERR_CHANNEL:
        return "This device has no such relay";
    case CAL_ERR_NO_READING:
        return "The meter has no reading yet; wait a few seconds";
    case CAL_ERR_LOW_VOLTAGE:
        return "The device reads under 80 V: calibrate on mains";
    case CAL_ERR_LOW_CURRENT:
        return "The load is too small to calibrate current: use one of at least 0.5 A";
    case CAL_ERR_LOW_POWER:
        return "The load is too small to calibrate power: use one of at least 50 W";
    case CAL_ERR_UNSTABLE:
        return "The load changed during the last few seconds: wait until it is steady";
    case CAL_ERR_RANGE:
        return "That is more than 15% from the built-in calibration: check the values and the load";
    }
    return "Unknown error";
}

cal_result_t cal_compute(const cal_ratios_t *current, int channels, int ch, const cal_values_t *measured,
                         bool stable, const cal_values_t *reference, cal_ratios_t *out)
{
    if (channels < 1 || channels > CAL_CHANNELS || ch < 0 || ch >= channels) {
        return CAL_ERR_CHANNEL;
    }
    bool gv = given(reference->volts), ga = given(reference->amps), gw = given(reference->watts);
    if (!gv && !ga && !gw) {
        return CAL_ERR_NOTHING;
    }
    if ((gv && !(isfinite(reference->volts) && reference->volts > 0.0f)) ||
        (ga && !(isfinite(reference->amps) && reference->amps > 0.0f)) ||
        (gw && !(isfinite(reference->watts) && reference->watts > 0.0f))) {
        return CAL_ERR_INPUT;
    }
    if ((gv && !isfinite(measured->volts)) || (ga && !isfinite(measured->amps)) ||
        (gw && !isfinite(measured->watts))) {
        return CAL_ERR_NO_READING;
    }
    if (gv && measured->volts < CAL_MIN_VOLTS) {
        return CAL_ERR_LOW_VOLTAGE;
    }
    if (ga && measured->amps < CAL_MIN_AMPS) {
        return CAL_ERR_LOW_CURRENT;
    }
    if (gw && measured->watts < CAL_MIN_WATTS) {
        return CAL_ERR_LOW_POWER;
    }
    if (!stable) {
        return CAL_ERR_UNSTABLE;
    }

    cal_ratios_t r = *current;
    if (gv) {
        r.q[CAL_Q_V] = current->q[CAL_Q_V] * reference->volts / measured->volts;
    }
    if (ga) {
        r.q[CAL_Q_I(ch)] = current->q[CAL_Q_I(ch)] * reference->amps / measured->amps;
    }
    if (gw) {
        r.q[CAL_Q_P(ch)] = current->q[CAL_Q_P(ch)] * reference->watts / measured->watts;
    } else {
        r.q[CAL_Q_P(ch)] = current->q[CAL_Q_P(ch)] * (r.q[CAL_Q_V] / current->q[CAL_Q_V]) *
                           (r.q[CAL_Q_I(ch)] / current->q[CAL_Q_I(ch)]);
    }
    if (!cal_ratios_valid(&r)) {
        return CAL_ERR_RANGE;
    }
    *out = r;
    return CAL_OK;
}

bool cal_rebase(const cal_ratios_t *stored, const float base_old[CAL_Q_COUNT], const float base_new[CAL_Q_COUNT],
                cal_ratios_t *out)
{
    bool all = true;
    for (int i = 0; i < CAL_Q_COUNT; i++) {
        bool bases = isfinite(base_old[i]) && base_old[i] > 0.0f && isfinite(base_new[i]) && base_new[i] > 0.0f;
        float r = bases ? stored->q[i] * base_old[i] / base_new[i] : 1.0f;
        if (!bases || !in_bounds(r)) {
            r = 1.0f;
            all = false;
        }
        out->q[i] = r;
    }
    return all;
}

// One form value: empty is "not given" (NAN). False if it is not a number.
static bool parse_value(const char *s, size_t n, float *out)
{
    if (n == 0) {
        *out = NAN;
        return true;
    }
    char buf[24];
    if (n >= sizeof(buf)) {
        return false;
    }
    memcpy(buf, s, n);
    buf[n] = '\0';
    char *end = NULL;
    float v = strtof(buf, &end);
    if (end == buf || *end != '\0') {
        return false;
    }
    *out = v;
    return true;
}

bool cal_parse_form(const char *text, int *ch, cal_values_t *reference)
{
    *ch = -1;
    reference->volts = reference->amps = reference->watts = NAN;
    const char *p = text;
    while (*p != '\0') {
        const char *end = strchr(p, '&');
        size_t len = end != NULL ? (size_t)(end - p) : strlen(p);
        const char *eq = memchr(p, '=', len);
        if (eq == NULL) {
            return false;
        }
        size_t klen = (size_t)(eq - p);
        const char *val = eq + 1;
        size_t vlen = len - klen - 1;
        float v;
        if (!parse_value(val, vlen, &v)) {
            return false;
        }
        if (klen == 2 && strncmp(p, "ch", 2) == 0) {
            if (isnan(v) || v != floorf(v) || v < 1.0f || v > 9.0f) {
                return false;
            }
            *ch = (int)v - 1;
        } else if (klen == 1 && p[0] == 'v') {
            reference->volts = v;
        } else if (klen == 1 && p[0] == 'a') {
            reference->amps = v;
        } else if (klen == 1 && p[0] == 'w') {
            reference->watts = v;
        } else {
            return false;
        }
        p += len;
        if (*p == '&') {
            p++;
        }
    }
    return *ch >= 0;
}

static uint32_t fnv1a(const void *data, size_t n)
{
    const uint8_t *b = (const uint8_t *)data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ b[i]) * 16777619u;
    }
    return h;
}

void cal_record_make(cal_record_t *rec, const float base[CAL_Q_COUNT], const cal_ratios_t *ratios)
{
    memset(rec, 0, sizeof(*rec));
    rec->magic = CAL_RECORD_MAGIC;
    rec->version = CAL_RECORD_VERSION;
    for (int i = 0; i < CAL_Q_COUNT; i++) {
        rec->base[i] = base[i];
        rec->ratio[i] = ratios->q[i];
    }
    rec->check = fnv1a(rec, offsetof(cal_record_t, check));
}

bool cal_record_valid(const cal_record_t *rec)
{
    if (rec->magic != CAL_RECORD_MAGIC || rec->version != CAL_RECORD_VERSION ||
        rec->check != fnv1a(rec, offsetof(cal_record_t, check))) {
        return false;
    }
    cal_ratios_t r;
    for (int i = 0; i < CAL_Q_COUNT; i++) {
        r.q[i] = rec->ratio[i];
    }
    return cal_ratios_valid(&r);
}
