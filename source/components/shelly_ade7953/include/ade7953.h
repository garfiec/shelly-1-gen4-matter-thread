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


// ade7953.h
//
// The ADE7953 power meter over I2C (CONFIG_SHELLY_HAS_ADE7953), for the
// Shelly 2PM Gen4. Only the metering task calls it.
//
// The only writes are the datasheet's setup (ade7953_codec.h), sent at init
// and again whenever a check finds the chip has lost it (a brownout resets
// the chip but not the ESP32). The reset-on-read registers are never read,
// and the chip's reset and interrupt pins are left alone. Every bus
// operation times out after 50 ms.

#pragma once

#include <esp_err.h>
#include <stdbool.h>

#include "ade7953_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

// Set up the bus and the chip. On any failure the bus stays installed and
// ade7953_read() keeps trying: the meter-lost alert covers a chip that
// never answers, and the relays keep working.
esp_err_t ade7953_init(void);

// Read one sample (VRMS, both IRMS and WATT, PERIOD). ESP_OK only if every
// register read. Also re-checks the setup every 10 s, and on a normal boot
// copies the registers into the hardware snapshot every minute.
esp_err_t ade7953_read(ade7953_raw_t *raw);

// The calibration from Kconfig (CONFIG_SHELLY_ADE7953_*_MUL).
const ade7953_cal_t *ade7953_calibration(void);

#ifdef __cplusplus
}
#endif
