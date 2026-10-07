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

// bl0942.h
//
// BL0942 energy meter over UART (CONFIG_SHELLY_BL0942_UART_NUM, 9600 8N1).
// Only built into firmware for boards with CONFIG_SHELLY_HAS_BL0942.

#pragma once

#include <esp_err.h>
#include <stdbool.h>

#include "bl0942_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

// Install the UART, then configure the MODE register for the given line
// frequency and verify it by reading it back. Returns ESP_OK if the chip
// answered with the expected MODE. On any other result the UART stays
// installed and bl0942_read() can still be tried: a chip that answers
// frames later is still usable.
esp_err_t bl0942_init(bool line_60hz);

// Reconfigure the line frequency (after measuring it). Same verification
// as bl0942_init().
esp_err_t bl0942_set_line_frequency(bool line_60hz);

// Request and read one full frame. Returns ESP_OK with a checksum-valid
// frame, ESP_ERR_TIMEOUT if nothing (or too little) arrived, or
// ESP_ERR_INVALID_CRC / ESP_ERR_INVALID_RESPONSE for a corrupt frame.
// Blocks for at most about 100ms.
esp_err_t bl0942_read(bl0942_raw_t *raw);

// The calibration from Kconfig (CONFIG_SHELLY_BL0942_*_REF).
const bl0942_cal_t *bl0942_calibration(void);

#ifdef __cplusplus
}
#endif
