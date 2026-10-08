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

// shelly_app.h
//
// Application plumbing shared by every variant: NVS bring-up, the Matter
// event and Identify callbacks, GeneralDiagnostics (boot reason, hardware
// faults), the liveness watchdog and OTA image confirmation.

#pragma once

#include <esp_err.h>
#include <esp_matter.h>

#include <ui_policy.h>

// Initialize NVS. If the partition is full or was written by a newer NVS
// format, erase it and initialize again, so the device comes back up
// ready to commission instead of boot-looping. Call first in app_main().
esp_err_t shelly_nvs_init(void);

// The Matter device event callback. Pass to esp_matter::start().
void shelly_app_event_cb(const chip::DeviceLayer::ChipDeviceEvent *event, intptr_t arg);

// Set the status LED's base state from the pairing window, the fabrics and
// Thread. Call on the Matter thread or with the CHIP stack lock held.
void shelly_led_refresh(void);

// The Identify callback. Pass to node::create(). Blinks the status LED.
esp_err_t shelly_app_identification_cb(esp_matter::identification::callback_type_t type, uint16_t endpoint_id,
                                       uint8_t effect_id, uint8_t effect_variant, void *priv_data);

// Add BootReason and ActiveHardwareFaults, and the HardwareFaultChange
// event, to GeneralDiagnostics on the root endpoint. Adds attributes
// only; creates no endpoint or cluster. Call after node::create().
esp_err_t shelly_diagnostics_add_attributes(esp_matter::node_t *node);

// Install the diagnostics provider that reports the boot reason and the
// hardware-fault registry. Must be called before esp_matter::start(): the
// GeneralDiagnostics boot event reads the provider at startup.
void shelly_diagnostics_install(void);

// Call right after esp_matter::start() succeeds. Publishes faults raised
// during boot and starts the health task: it feeds the task watchdog
// while the Matter thread stays responsive, and confirms a freshly
// installed OTA image after 60s of healthy running.
void shelly_app_started(void);

// True once shelly_app_started() has run.
bool shelly_app_is_started(void);

// The CHIP stack lock for the input tasks (button, wall switch). It waits at
// most timeout_ms, so a busy or wedged Matter thread refuses the input (the
// caller shows LED-14) instead of freezing the task: locked() says which. A
// lock this task already holds counts as locked. Only after Matter started.
#define SHELLY_INPUT_LOCK_TIMEOUT_MS 2000

class ShellyInputLock
{
public:
    explicit ShellyInputLock(uint32_t timeout_ms);
    ~ShellyInputLock();
    bool locked() const { return m_locked; }

    ShellyInputLock(const ShellyInputLock &) = delete;
    ShellyInputLock &operator=(const ShellyInputLock &) = delete;

private:
    bool m_locked = false;
    bool m_owned = false;
};

// Toggle or set the OnOff attribute on an endpoint. The attribute update
// runs the normal PRE_UPDATE path, which drives the relay. No-op before
// Matter starts or while endpoint_id is still 0. Takes the CHIP stack
// lock; do not call while holding the relay lock.
//
// shelly_onoff_toggle() is for local inputs (button, wall switch). It runs
// the local-toggle hook, if one is set, with the value it is about to flip,
// under the same lock as the read and the write, and waits for that lock at
// most SHELLY_INPUT_LOCK_TIMEOUT_MS.
// Returns true if the OnOff attribute changed; false if the relay refused
// (thermal lockout, protection), Matter is not up yet or stayed busy.
bool shelly_onoff_toggle(uint16_t endpoint_id);
// The same, but to a given state: the wall switch's Follow mode. If OnOff
// is already `on`, nothing is written, but the hooks still run and it
// returns whether the relay is in that state.
bool shelly_onoff_toggle_to(uint16_t endpoint_id, bool on);
void shelly_onoff_set(uint16_t endpoint_id, bool on);

// Called by shelly_onoff_toggle() and shelly_onoff_toggle_to() around every
// local toggle or set, while holding the CHIP stack lock, and by their
// shelly_local_relay_* counterparts in maintenance mode, without it: hook
// before the write, with the state the user asked for (also when the
// output is already there and nothing is written), and done after it, with
// whether the output is now in that state. Neither may block. Set during
// boot, before Matter starts.
typedef void (*shelly_local_toggle_hook_t)(bool turning_on);
typedef void (*shelly_local_toggle_done_t)(bool accepted);
void shelly_onoff_set_local_toggle_hook(shelly_local_toggle_hook_t hook, shelly_local_toggle_done_t done);

// Set the OnOff attribute on the Matter thread, without waiting for the
// CHIP stack lock. For protection code that must never block on Matter.
void shelly_onoff_set_async(uint16_t endpoint_id, bool on);

// ---------------------------------------------------------------------------
// Device UI: the button and the wall switch
// ---------------------------------------------------------------------------
//
// Every input and what it does in every state is specified in
// docs/BUTTON-AND-LED.md, decided by ui_decide() (ui_policy.h) and carried
// out here. Change them together (CLAUDE.md).

// Toggle the output; true if it changed, false if refused.
typedef bool (*shelly_ui_toggle_fn_t)(void);
// Bring the output to `on` by a toggle if it differs; true if it is now
// there, false if refused.
typedef bool (*shelly_ui_toggle_to_fn_t)(bool on);

typedef struct {
    // Tap and toggle-role wall switch in normal mode: normally
    // shelly_onoff_toggle() on the relay endpoint (the Opener's OnOff write
    // runs its pulse).
    shelly_ui_toggle_fn_t toggle;
    // The same without Matter, for maintenance mode: the relay directly
    // (the Opener pulses it).
    shelly_ui_toggle_fn_t local_toggle;
    // Toggle role only (Follow mode): the same as toggle / local_toggle,
    // but to a given state. NULL for the Opener and the Light Switch.
    shelly_ui_toggle_to_fn_t toggle_to;
    shelly_ui_toggle_to_fn_t local_toggle_to;
    // What the wall switch terminal does on this variant.
    ui_switch_role_t switch_role;
    // The variant's own handling of a wall-switch change (UI_VARIANT): the
    // Opener's contact report, the Light Switch's bound command. NULL for
    // the toggle role.
    void (*switch_handler)(void);
    // Relays on the board: 0 or 1 for one, 2 for the 2PM. With 2, two taps
    // and the second wall switch use the functions below for relay 2.
    uint8_t channels;
    shelly_ui_toggle_fn_t toggle2;
    shelly_ui_toggle_fn_t local_toggle2;
    shelly_ui_toggle_to_fn_t toggle_to2;
    shelly_ui_toggle_to_fn_t local_toggle_to2;
} shelly_ui_config_t;

// Start the button (button_init) with the device UI. Call during boot,
// after the status LED and the relay. The config is copied.
esp_err_t shelly_ui_init(const shelly_ui_config_t *config);

// Every variant calls this from its wall-switch callback (switch_input), so
// ui_decide() decides what a change does in each state and switch mode.
// active: the switch is now on or pressed (switch_input_active()).
void shelly_ui_switch_changed(bool active);
// The same for one input of a 2-channel board (0 = SW1).
void shelly_ui_switch_changed_ch(int ch, bool active);

// The variant's wall-switch role (from shelly_ui_init).
ui_switch_role_t shelly_ui_switch_role(void);

// Toggle the relay directly, without Matter: the local toggle for Wi-Fi
// maintenance mode. Runs the local-toggle hook first, like
// shelly_onoff_toggle(). True if the relay accepted the change.
bool shelly_local_relay_toggle(void);
bool shelly_local_relay_toggle_to(bool on);
// The same for one relay (0 = relay 1).
bool shelly_local_relay_toggle_ch(int ch);
bool shelly_local_relay_toggle_to_ch(int ch, bool on);

// Which relay channel an OnOff endpoint drives. Every variant's relay
// endpoints default to channel 0; a 2-channel variant registers its second.
void shelly_relay_endpoint_register(int ch, uint16_t endpoint_id);
int shelly_relay_endpoint_channel(uint16_t endpoint_id);

// ---------------------------------------------------------------------------
// Wi-Fi maintenance mode (shelly_maintenance component)
// ---------------------------------------------------------------------------

// Record a request to restart into maintenance mode, with the current relay
// state. The caller restarts. ESP_ERR_NOT_SUPPORTED without
// CONFIG_SHELLY_MAINTENANCE.
esp_err_t shelly_maintenance_request(void);

// Call once in app_main, right after NVS and the status LED. True if this
// boot is a maintenance boot: then run the maintenance mode
// (shelly_maintenance_run) instead of starting Matter. The request is
// consumed either way, so a crash in maintenance mode comes back normally.
bool shelly_maintenance_boot_pending(bool *relay_was_on);
// The same with every relay: bit n set means relay n+1 was on.
bool shelly_maintenance_boot_pending_mask(uint32_t *relays_on);

// True during a maintenance boot.
bool shelly_maintenance_active(void);

// Record the relay state before a restart out of maintenance mode into
// normal mode (the Exit button, the 5 s hold, the idle timeout, an update).
void shelly_maintenance_exit_record(void);

// Call in a normal boot once the relay's OnOff endpoint exists, before
// esp_matter::start(). After a restart out of maintenance mode, it stores
// the relay state recorded there as the OnOff value, so StartUpOnOff
// applies to the state the relay really had (P-03) instead of the one from
// before maintenance mode. The Opener, momentary, never calls it.
void shelly_maintenance_carry_relay(uint16_t endpoint_id);
// The same for every relay at once (endpoint_ids[n] is relay n+1's). The
// record is consumed by the first carry call, so a 2-channel variant must
// use this one.
void shelly_maintenance_carry_relays(const uint16_t *endpoint_ids, int count);

// Internal: run by shelly_app_started(). Performs a factory reset that was
// requested before Matter was up (see shelly_ui.cpp).
void shelly_ui_matter_started(void);
