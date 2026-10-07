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

// protection_logic.c

#include "protection_logic.h"

#include <math.h>

// ---------------------------------------------------------------------------
// I²t
// ---------------------------------------------------------------------------

void i2t_init(i2t_t *m, float pickup_a, float k_a2s, float tau_s)
{
    m->pickup_a = pickup_a;
    m->k_a2s = k_a2s;
    m->tau_s = tau_s;
    m->heat_a2s = 0.0f;
}

bool i2t_update(i2t_t *m, float amps, float dt_s)
{
    if (dt_s < 0.0f) {
        dt_s = 0.0f;
    }
    if (amps > m->pickup_a) {
        m->heat_a2s += (amps * amps - m->pickup_a * m->pickup_a) * dt_s;
    } else if (m->tau_s > 0.0f) {
        m->heat_a2s *= expf(-dt_s / m->tau_s);
    }
    return m->heat_a2s >= m->k_a2s;
}

float i2t_trip_time_s(const i2t_t *m, float amps)
{
    float excess = amps * amps - m->pickup_a * m->pickup_a;
    if (excess <= 0.0f) {
        return -1.0f;
    }
    return m->k_a2s / excess;
}

// ---------------------------------------------------------------------------
// Nominal voltage and overvoltage
// ---------------------------------------------------------------------------

void nominal_classifier_init(nominal_classifier_t *c, float present_v, int settle_updates, float split_v)
{
    c->present_v = present_v;
    c->settle_updates = settle_updates;
    c->split_v = split_v;
    c->present_count = 0;
}

mains_nominal_t nominal_classifier_update(nominal_classifier_t *c, float volts)
{
    // The BL0942 reports 0 V until its first RMS update after power-up;
    // those frames, and any dip below present_v, restart the count.
    if (!(volts >= c->present_v)) {
        c->present_count = 0;
        return MAINS_NOMINAL_UNKNOWN;
    }
    if (++c->present_count < c->settle_updates) {
        return MAINS_NOMINAL_UNKNOWN;
    }
    return volts < c->split_v ? MAINS_NOMINAL_120V : MAINS_NOMINAL_240V;
}

void overvoltage_init(overvoltage_t *o, float limit_v, int64_t sustain_ms, int64_t recover_ms)
{
    o->limit_v = limit_v;
    o->sustain_ms = sustain_ms;
    o->recover_ms = recover_ms;
    o->over_since_ms = -1;
    o->ok_since_ms = -1;
    o->tripped = false;
}

float overvoltage_limit_for(mains_nominal_t nominal)
{
    switch (nominal) {
    case MAINS_NOMINAL_120V:
        return 150.0f;
    case MAINS_NOMINAL_240V:
        return 264.0f;
    default:
        return 0.0f;
    }
}

ov_event_t overvoltage_update(overvoltage_t *o, float volts, int64_t now_ms)
{
    if (o->limit_v <= 0.0f) {
        return OV_EVENT_NONE;
    }

    if (volts > o->limit_v) {
        o->ok_since_ms = -1;
        if (o->over_since_ms < 0) {
            o->over_since_ms = now_ms;
        }
        if (!o->tripped && now_ms - o->over_since_ms >= o->sustain_ms) {
            o->tripped = true;
            return OV_EVENT_TRIP;
        }
        return OV_EVENT_NONE;
    }

    o->over_since_ms = -1;
    if (o->ok_since_ms < 0) {
        o->ok_since_ms = now_ms;
    }
    if (o->tripped && now_ms - o->ok_since_ms >= o->recover_ms) {
        o->tripped = false;
        return OV_EVENT_RECOVER;
    }
    return OV_EVENT_NONE;
}

// ---------------------------------------------------------------------------
// Welded relay
// ---------------------------------------------------------------------------

void welded_init(welded_t *w, float alarm_a, float clear_a, int64_t off_settle_ms, int64_t alarm_ms, int64_t clear_ms)
{
    w->alarm_a = alarm_a;
    w->clear_a = clear_a;
    w->off_settle_ms = off_settle_ms;
    w->alarm_ms = alarm_ms;
    w->clear_ms = clear_ms;
    w->off_since_ms = -1;
    w->high_since_ms = -1;
    w->low_since_ms = -1;
    w->alarmed = false;
}

bool welded_update(welded_t *w, bool commanded_on, float amps, int64_t now_ms)
{
    bool was = w->alarmed;

    if (commanded_on) {
        // A successful On/Off cycle clears the alarm; commanded on, current
        // is expected, so nothing is timed.
        w->off_since_ms = -1;
        w->high_since_ms = -1;
        w->low_since_ms = -1;
        w->alarmed = false;
        return w->alarmed != was;
    }

    if (w->off_since_ms < 0) {
        w->off_since_ms = now_ms;
    }
    bool settled = now_ms - w->off_since_ms >= w->off_settle_ms;

    if (settled && amps >= w->alarm_a) {
        if (w->high_since_ms < 0) {
            w->high_since_ms = now_ms;
        }
        if (now_ms - w->high_since_ms >= w->alarm_ms) {
            w->alarmed = true;
        }
    } else {
        w->high_since_ms = -1;
    }

    if (amps < w->clear_a) {
        if (w->low_since_ms < 0) {
            w->low_since_ms = now_ms;
        }
        if (w->alarmed && now_ms - w->low_since_ms >= w->clear_ms) {
            w->alarmed = false;
        }
    } else {
        w->low_since_ms = -1;
    }

    return w->alarmed != was;
}

// ---------------------------------------------------------------------------
// AC presence
// ---------------------------------------------------------------------------

void ac_detect_init(ac_detect_t *a, float min_v, int required)
{
    a->min_v = min_v;
    a->required = required;
    a->count = 0;
    a->present = false;
}

bool ac_detect_update(ac_detect_t *a, float volts)
{
    if (volts >= a->min_v) {
        if (a->count < a->required) {
            a->count++;
        }
    } else {
        a->count = 0;
    }
    if (a->count >= a->required) {
        a->present = true;
    }
    return a->present;
}
