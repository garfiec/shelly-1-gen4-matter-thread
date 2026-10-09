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

// metering_logic.c

#include "metering_logic.h"

#include <math.h>

void energy_acc_init(energy_acc_t *e, double mwh_per_pulse, float max_w, double slack_mwh)
{
    e->mwh_per_pulse = mwh_per_pulse;
    e->max_w = max_w;
    e->slack_mwh = slack_mwh;
    e->total_mwh = 0.0;
    e->last_cf = 0;
    e->last_ms = 0;
    e->have_last = false;
    e->first_after_restore = false;
}

void energy_acc_restore(energy_acc_t *e, int64_t total_mwh, uint32_t last_cf, int64_t now_ms)
{
    e->total_mwh = (double)total_mwh;
    e->last_cf = last_cf;
    e->last_ms = now_ms;
    e->have_last = true;
    e->first_after_restore = true;
}

energy_result_t energy_acc_update(energy_acc_t *e, uint32_t cf_cnt, int64_t now_ms)
{
    if (!e->have_last) {
        e->last_cf = cf_cnt;
        e->last_ms = now_ms;
        e->have_last = true;
        return ENERGY_BASELINE;
    }

    bool after_restore = e->first_after_restore;
    e->first_after_restore = false;

    if (cf_cnt < e->last_cf) {
        e->last_cf = cf_cnt;
        e->last_ms = now_ms;
        return ENERGY_CHIP_RESET;
    }

    int64_t dt_ms = now_ms - e->last_ms;
    if (dt_ms < 0) {
        dt_ms = 0;
    }
    double delta_mwh = (double)(cf_cnt - e->last_cf) * e->mwh_per_pulse;
    // max_w over dt, in mWh, plus one pulse for counter granularity.
    double cap_mwh = (double)e->max_w * (double)dt_ms / 3600.0 + e->mwh_per_pulse;
    if (after_restore) {
        cap_mwh += e->slack_mwh;
    }

    e->last_cf = cf_cnt;
    e->last_ms = now_ms;
    if (delta_mwh > cap_mwh) {
        return ENERGY_GLITCH;
    }
    e->total_mwh += delta_mwh;
    return ENERGY_ADDED;
}

int64_t energy_acc_total_mwh(const energy_acc_t *e)
{
    return (int64_t)e->total_mwh;
}

void energy_save_policy_init(energy_save_policy_t *p, int64_t save_delta_mwh, int64_t save_interval_ms,
                             int64_t saved_mwh, int64_t now_ms)
{
    p->save_delta_mwh = save_delta_mwh;
    p->save_interval_ms = save_interval_ms;
    p->saved_mwh = saved_mwh;
    p->saved_ms = now_ms;
}

bool energy_save_due(const energy_save_policy_t *p, int64_t total_mwh, int64_t now_ms)
{
    if (total_mwh == p->saved_mwh) {
        return false;
    }
    return (total_mwh - p->saved_mwh >= p->save_delta_mwh) || (now_ms - p->saved_ms >= p->save_interval_ms);
}

void energy_save_done(energy_save_policy_t *p, int64_t total_mwh, int64_t now_ms)
{
    p->saved_mwh = total_mwh;
    p->saved_ms = now_ms;
}

bool report_changed(float last, float now, float abs_step, float rel_step)
{
    if (isnan(last) || isnan(now)) {
        return isnan(last) != isnan(now);
    }
    float step = fmaxf(abs_step, fabsf(last) * rel_step);
    return fabsf(now - last) >= step;
}

void noload_cut(float *amps, float *watts, float min_w, float max_noise_a)
{
    if (fabsf(*watts) < min_w && *amps < max_noise_a) {
        *amps = 0.0f;
        *watts = 0.0f;
    }
}

double power_to_mwh(float watts, int64_t dt_ms, int64_t max_dt_ms)
{
    if (dt_ms <= 0 || !(watts > 0.0f)) {
        return 0.0;
    }
    if (dt_ms > max_dt_ms) {
        dt_ms = max_dt_ms;
    }
    return (double)watts * (double)dt_ms / 3600.0;
}

void avg_window_init(avg_window_t *w, int size)
{
    w->size = size < 1 ? 1 : (size > AVG_WINDOW_MAX ? AVG_WINDOW_MAX : size);
    w->count = 0;
    w->next = 0;
}

void avg_window_push(avg_window_t *w, float x)
{
    if (!isfinite(x)) {
        w->count = 0;
        w->next = 0;
        return;
    }
    w->v[w->next] = x;
    w->next = (w->next + 1) % w->size;
    if (w->count < w->size) {
        w->count++;
    }
}

bool avg_window_full(const avg_window_t *w)
{
    return w->count == w->size;
}

float avg_window_mean(const avg_window_t *w)
{
    if (w->count == 0) {
        return NAN;
    }
    double sum = 0.0;
    for (int i = 0; i < w->count; i++) {
        sum += w->v[i];
    }
    return (float)(sum / w->count);
}

float avg_window_spread(const avg_window_t *w)
{
    float mean = avg_window_mean(w);
    if (!(mean > 0.0f)) {
        return INFINITY;
    }
    float lo = w->v[0], hi = w->v[0];
    for (int i = 1; i < w->count; i++) {
        lo = fminf(lo, w->v[i]);
        hi = fmaxf(hi, w->v[i]);
    }
    return (hi - lo) / mean;
}
