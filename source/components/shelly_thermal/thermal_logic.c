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

// thermal_logic.c

#include "thermal_logic.h"

#include <math.h>

// Above this fraction of the supply the NTC reads as open. 0.95 is about
// 190 kOhm, or -33°C with a 10k/3350 NTC.
#define NTC_OPEN_FRACTION   0.95f
// Clamp to the Temperature Sensor endpoint's range. A reading at the low
// rail (a short) clamps to the top, far past any trip.
#define NTC_MAX_C           150.0f
#define NTC_MIN_C           -40.0f
#define KELVIN              273.15f

void thermal_hyst_init(thermal_hyst_t *h, float trip_c, float clear_c, int64_t sustain_ms)
{
    h->trip_c = trip_c;
    h->clear_c = clear_c;
    h->sustain_ms = sustain_ms;
    h->over_since_ms = -1;
    h->tripped = false;
}

bool thermal_hyst_update(thermal_hyst_t *h, float temp_c, int64_t now_ms)
{
    bool was = h->tripped;

    if (temp_c > h->trip_c) {
        if (h->over_since_ms < 0) {
            h->over_since_ms = now_ms;
        }
        if (now_ms - h->over_since_ms >= h->sustain_ms) {
            h->tripped = true;
        }
    } else {
        h->over_since_ms = -1;
        if (temp_c < h->clear_c) {
            h->tripped = false;
        }
    }
    return h->tripped != was;
}

ntc_status_t ntc_temperature_c(const ntc_params_t *p, float v_mv, float *temp_c)
{
    if (v_mv >= p->vref_mv * NTC_OPEN_FRACTION) {
        return NTC_READING_OPEN;
    }
    if (v_mv <= 0.0f) {
        *temp_c = NTC_MAX_C;
        return NTC_READING_OK;
    }

    // Divider: v = vref * r_ntc / (r_fixed + r_ntc)
    float r_ntc = p->r_fixed_ohm * v_mv / (p->vref_mv - v_mv);
    float inv_t = 1.0f / (p->t_nominal_c + KELVIN) + logf(r_ntc / p->r_nominal_ohm) / p->beta;
    float t = 1.0f / inv_t - KELVIN;

    if (t > NTC_MAX_C || !(t == t)) {
        t = NTC_MAX_C;
    } else if (t < NTC_MIN_C) {
        t = NTC_MIN_C;
    }
    *temp_c = t;
    return NTC_READING_OK;
}
