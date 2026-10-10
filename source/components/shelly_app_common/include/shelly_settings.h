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

// shelly_settings.h
//
// The device settings changed on the Wi-Fi maintenance page: the Matter
// setup code (shelly_setup_code.h), the wall-switch mode, the Wi-Fi mode
// password (shelly_wifi_password.h), on metered boards the meter
// calibration (shelly_calibration.h) and, where the build offers it, the
// device type (device_profile.h). A change made on the page waits here
// until the button is pressed 3 times within SHELLY_CHANGE_CONFIRM_S
// (docs/BUTTON-AND-LED.md, B-26), so nobody who joins the network can
// change a setting without the device in hand.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <calibration_logic.h>
#include <device_profile.h>
#include <setup_code.h>
#include <ui_policy.h>
#include <wifi_password.h>

#ifdef __cplusplus
extern "C" {
#endif

// Load the saved settings. shelly_ui_init() calls it, after NVS is up.
void shelly_settings_init(void);

// The wall-switch mode (toggle role only). Toggle if none is saved or the
// saved value is unknown.
ui_switch_mode_t shelly_switch_mode_get(void);
// The same for one input (0 = SW1). Each input of a 2-channel board has its own.
ui_switch_mode_t shelly_switch_mode_get_ch(int ch);

// The device type this boot runs as: read once, the first time it's asked
// for (app_main asks before anything else uses the relay), and kept for the
// whole boot, since a saved change applies from the next boot. Outlet if
// the build doesn't offer the choice, has no Wi-Fi mode, or none is saved.
device_profile_t shelly_profile_get(void);
// The saved device type: the one the next boot runs as.
device_profile_t shelly_profile_saved(void);
// Whether this build offers the choice (CONFIG_SHELLY_HAS_OPENER_MODE).
bool shelly_profile_available(void);

// ---- The change waiting for 3 presses ----

#define SHELLY_CHANGE_CONFIRM_S 120

typedef enum {
    SHELLY_CHANGE_NONE = 0,
    SHELLY_CHANGE_SETUP_CODE,
    SHELLY_CHANGE_SWITCH_MODE,
    SHELLY_CHANGE_CALIBRATION,
    SHELLY_CHANGE_WIFI_PASSWORD,
    SHELLY_CHANGE_PROFILE,
} shelly_change_kind_t;

typedef struct {
    shelly_change_kind_t kind;
    setup_code_t code;              // SHELLY_CHANGE_SETUP_CODE
    ui_switch_mode_t switch_mode;   // SHELLY_CHANGE_SWITCH_MODE
    uint8_t channel;                // SHELLY_CHANGE_SWITCH_MODE: which input (0 = SW1);
                                    // SHELLY_CHANGE_CALIBRATION: which relay was measured
    cal_ratios_t cal;               // SHELLY_CHANGE_CALIBRATION: the new ratios
    char wifi_password[WIFI_PASSWORD_MAX + 1]; // SHELLY_CHANGE_WIFI_PASSWORD: the owner's
                                    // own, or "" to go back to the sticker code (or none)
    device_profile_t profile;       // SHELLY_CHANGE_PROFILE
} shelly_change_t;

// Stage a change. It replaces any change already waiting.
void shelly_change_stage(const shelly_change_t *change);

// Stage a switch-mode change. False if this variant's wall switch has no
// modes (the Opener, the Light Switch) or the mode is not valid.
bool shelly_switch_mode_stage(ui_switch_mode_t mode);
bool shelly_switch_mode_stage_ch(int ch, ui_switch_mode_t mode);

// Stage new meter calibration ratios (all 1.0 to go back to the built-in
// calibration). False if this board has no meter or a ratio is out of bounds.
bool shelly_cal_stage(int ch, const cal_ratios_t *ratios);

// Stage the Wi-Fi mode password: the owner's own, or "" for the default.
// False if it isn't 8 to 63 printable ASCII characters.
bool shelly_wifi_password_stage(const char *password);

// Stage a device type. False if this build doesn't offer the choice or the
// type is not valid.
bool shelly_profile_stage(device_profile_t profile);

void shelly_change_cancel(void);

// The waiting change and the seconds left to confirm it; false if none.
bool shelly_change_pending(shelly_change_t *out, uint32_t *secs_left);

// Save the waiting change: a setup code and a device type apply from the
// next normal boot, a Wi-Fi mode password from the next Wi-Fi mode, a
// switch mode and a calibration at once. False if none is waiting or the save failed.
bool shelly_change_confirm(void);

// How many changes have been saved this boot: the page compares it to tell
// a saved change from one that timed out.
uint32_t shelly_change_saved_count(void);

#ifdef __cplusplus
}
#endif
