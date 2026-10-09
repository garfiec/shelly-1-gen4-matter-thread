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

// shelly_ui.cpp
//
// Carries out the device UI: ui_decide() says what an input does in the
// current state, and this file does it. docs/BUTTON-AND-LED.md is the
// specification; keep them in step (CLAUDE.md).

#include "shelly_app.h"

#include <esp_attr.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>

#include <button.h>
#include <status_led.h>

#include <app/clusters/ota-requestor/OTARequestorInterface.h>
#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>

#include <relay.h>
#include <shelly_diag_log.h>
#include <shelly_settings.h>

using namespace esp_matter;

static const char *TAG = "shelly_ui";

// Pairing window opened by 3 presses. A paired device opens it for a second
// ecosystem; an unpaired one gets as long as the window it opens at boot.
#define PAIRING_WINDOW_PAIRED_S     180
#define PAIRING_WINDOW_UNPAIRED_S   CONFIG_CHIP_DISCOVERY_TIMEOUT_SECS
// Let the accepted flash show before a restart.
#define RESTART_FLASH_MS            700

static shelly_ui_config_t s_config;

// ---------------------------------------------------------------------------
// Deferred factory reset
// ---------------------------------------------------------------------------
//
// esp_matter::factory_reset() needs the Matter server running. Before Matter
// starts, and in maintenance mode, a reset is recorded here, the device
// restarts, and shelly_ui_matter_started() performs it. RTC_NOINIT memory
// survives the software restart; after a power cycle it holds garbage, so a
// record is only trusted with its magic and check, and never after power-on.

#define RESET_RECORD_MAGIC 0x52535450u  // 'RSTP'

typedef struct {
    uint32_t magic;
    uint32_t check;
} reset_record_t;

static RTC_NOINIT_ATTR reset_record_t s_reset_record;

static bool reset_record_valid(void)
{
    return s_reset_record.magic == RESET_RECORD_MAGIC && s_reset_record.check == ~RESET_RECORD_MAGIC &&
           esp_reset_reason() != ESP_RST_POWERON;
}

static void reset_record_clear(void)
{
    s_reset_record.magic = 0;
    s_reset_record.check = 0;
}

static void restart_after_flash(void)
{
    vTaskDelay(pdMS_TO_TICKS(RESTART_FLASH_MS));
    esp_restart();
}

static void meter_guard_clean_exit(void);

static void defer_factory_reset(void)
{
    ESP_LOGW(TAG, "Factory reset requested before Matter is up: restarting to do it");
    meter_guard_clean_exit();
    s_reset_record.magic = RESET_RECORD_MAGIC;
    s_reset_record.check = ~RESET_RECORD_MAGIC;
    status_led_flash_accepted();
    restart_after_flash();
}

static void factory_reset_work(intptr_t arg)
{
    ESP_LOGW(TAG, "Factory reset");
    esp_matter::factory_reset();
}

void shelly_ui_matter_started(void)
{
    if (reset_record_valid()) {
        reset_record_clear();
        ESP_LOGW(TAG, "Performing the factory reset requested before the restart");
        if (chip::DeviceLayer::PlatformMgr().ScheduleWork(factory_reset_work, 0) != CHIP_NO_ERROR) {
            ESP_LOGE(TAG, "Failed to schedule the factory reset");
        }
        return;
    }
    reset_record_clear();
}

// ---------------------------------------------------------------------------
// Wi-Fi maintenance mode request, and the way back
// ---------------------------------------------------------------------------
//
// The 5 s hold records a request here and restarts; app_main then runs the
// maintenance boot (shelly_maintenance component). Leaving that boot for
// normal mode records the relay state in the same slot, for the normal boot
// that follows. Like the reset record, it lives in RTC_NOINIT memory, and is
// honored only after a deliberate software restart. Every boot reads and
// clears it once (take_record), whichever variant and whether or not it
// uses it, so nothing stale is left for a later boot.

#define MAINT_RECORD_MAGIC 0x544E4D53u  // 'SMNT': enter maintenance mode
#define EXIT_RECORD_MAGIC  0x58454D53u  // 'SMEX': back from it, relay_on kept
#if CONFIG_SHELLY_MAINTENANCE
#define MAINTENANCE_BUILT 1
#else
#define MAINTENANCE_BUILT 0
#endif

typedef struct {
    uint32_t magic;
    uint32_t relay_on;
    uint32_t check;
} maint_record_t;

static RTC_NOINIT_ATTR maint_record_t s_maint_record;
static bool s_maintenance_active = false;

// What this boot found in the record: its magic if valid, else 0.
static bool s_record_taken = false;
static uint32_t s_record_found = 0;
// Bit n: relay n+1 was on. Bit 0 alone is what single-relay firmware wrote.
static uint32_t s_record_relays = 0;

static uint32_t maint_check(const maint_record_t *r)
{
    return ~(r->magic ^ (r->relay_on * 0x9E3779B9u));
}

static void record_store(uint32_t magic)
{
    s_maint_record.magic = magic;
    s_maint_record.relay_on = relay_get_mask();
    s_maint_record.check = maint_check(&s_maint_record);
}

static void meter_guard_forget_after_power_on(void);

static void take_record(void)
{
    if (s_record_taken) {
        return;
    }
    s_record_taken = true;
    meter_guard_forget_after_power_on();
    uint32_t magic = s_maint_record.magic;
    bool valid = (magic == MAINT_RECORD_MAGIC || magic == EXIT_RECORD_MAGIC) &&
                 s_maint_record.check == maint_check(&s_maint_record) && esp_reset_reason() == ESP_RST_SW;
    s_record_found = valid ? magic : 0;
    s_record_relays = s_maint_record.relay_on;
    // Consume it now: a crash in maintenance mode restarts normally.
    s_maint_record.magic = 0;
    s_maint_record.check = 0;
}

esp_err_t shelly_maintenance_request(void)
{
#if CONFIG_SHELLY_MAINTENANCE
    record_store(MAINT_RECORD_MAGIC);
    ESP_LOGW(TAG, "Restarting into Wi-Fi maintenance mode");
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

bool shelly_maintenance_boot_pending_mask(uint32_t *relays_on)
{
    take_record();
    if (s_record_found != MAINT_RECORD_MAGIC || !MAINTENANCE_BUILT) {
        return false;
    }
    s_maintenance_active = true;
    if (relays_on != nullptr) {
        *relays_on = s_record_relays;
    }
    return true;
}

bool shelly_maintenance_boot_pending(bool *relay_was_on)
{
    uint32_t relays = 0;
    if (!shelly_maintenance_boot_pending_mask(&relays)) {
        return false;
    }
    if (relay_was_on != nullptr) {
        *relay_was_on = (relays & 1u) != 0;
    }
    return true;
}

bool shelly_maintenance_active(void)
{
    return s_maintenance_active;
}

void shelly_maintenance_exit_record(void)
{
    meter_guard_clean_exit();
    record_store(EXIT_RECORD_MAGIC);
}

void shelly_maintenance_failed_record(void)
{
    record_store(EXIT_RECORD_MAGIC);
}

// ---------------------------------------------------------------------------
// The meter in Wi-Fi maintenance mode
// ---------------------------------------------------------------------------
//
// Wi-Fi mode is the only way to install firmware on a device without a
// serial cable, so the meter code it runs (for the calibration page) must
// never be able to take it down for good. A crash in Wi-Fi mode restarts
// into normal mode, which runs fine and confirms the image, so the 60 s
// rollback doesn't help, and holding 5 s again would crash again. This
// record marks a maintenance boot that started the meter; every clean way
// out clears the mark. Found still set, the meter stays off in Wi-Fi mode
// until the next power cycle. RTC memory may keep its contents through a
// short one, and the record is only read in maintenance boots, which are
// never power-on boots: the power-on boot forgets it (take_record()).

#define METER_GUARD_MAGIC 0x444D4D53u  // 'SMMD'

typedef struct {
    uint32_t magic;
    uint32_t running;       // A maintenance boot started the meter and hasn't left cleanly
    uint32_t died;          // One didn't: keep the meter off until a power cycle
    uint32_t check;
} meter_guard_t;

static RTC_NOINIT_ATTR meter_guard_t s_meter_guard;
static bool s_meter_skipped = false;

static uint32_t meter_guard_check(const meter_guard_t *g)
{
    return ~(g->magic ^ (g->running * 0x9E3779B9u) ^ (g->died * 0x85EBCA6Bu));
}

static bool meter_guard_valid(void)
{
    return s_meter_guard.magic == METER_GUARD_MAGIC && s_meter_guard.check == meter_guard_check(&s_meter_guard) &&
           esp_reset_reason() != ESP_RST_POWERON;
}

static void meter_guard_store(bool running, bool died)
{
    s_meter_guard.magic = METER_GUARD_MAGIC;
    s_meter_guard.running = running ? 1 : 0;
    s_meter_guard.died = died ? 1 : 0;
    s_meter_guard.check = meter_guard_check(&s_meter_guard);
}

static void meter_guard_forget_after_power_on(void)
{
    if (esp_reset_reason() == ESP_RST_POWERON) {
        s_meter_guard.magic = 0;
        s_meter_guard.check = 0;
    }
}

static void meter_guard_clean_exit(void)
{
    if (s_maintenance_active && meter_guard_valid()) {
        meter_guard_store(false, s_meter_guard.died != 0);
    }
}

bool shelly_maintenance_meter_allowed(void)
{
    bool died = meter_guard_valid() && (s_meter_guard.running != 0 || s_meter_guard.died != 0);
    if (died) {
        meter_guard_store(false, true);
        s_meter_skipped = true;
        ESP_LOGW(TAG, "The meter stays off in Wi-Fi mode: a Wi-Fi mode boot with it stopped unexpectedly "
                      "since power-on");
        return false;
    }
    meter_guard_store(true, false);
    return true;
}

bool shelly_maintenance_meter_skipped(void)
{
    return s_meter_skipped;
}

static void carry_one(uint16_t endpoint_id, bool on)
{
    // Stored before Matter starts and without callbacks: the relay itself
    // follows once StartUpOnOff has been applied to this value.
    attribute_t *attribute =
        attribute::get(endpoint_id, chip::app::Clusters::OnOff::Id, chip::app::Clusters::OnOff::Attributes::OnOff::Id);
    esp_matter_attr_val_t val = esp_matter_bool(on);
    esp_err_t err = attribute == nullptr ? ESP_ERR_NOT_FOUND : attribute::set_val(attribute, &val, false);
    if (err != ESP_OK && err != ESP_ERR_NOT_FINISHED) {
        ESP_LOGW(TAG, "Relay state from Wi-Fi maintenance mode not kept: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "Back from Wi-Fi maintenance mode, where the relay on endpoint %u was %s", endpoint_id,
             on ? "on" : "off");
}

void shelly_maintenance_carry_relays(const uint16_t *endpoint_ids, int count)
{
    take_record();
    if (s_record_found != EXIT_RECORD_MAGIC) {
        return;
    }
    s_record_found = 0;
    for (int ch = 0; ch < count && ch < 32; ch++) {
        if (endpoint_ids[ch] != 0) {
            carry_one(endpoint_ids[ch], (s_record_relays & (1u << ch)) != 0);
        }
    }
}

void shelly_maintenance_carry_relay(uint16_t endpoint_id)
{
    if (endpoint_id == 0) {
        return;
    }
    shelly_maintenance_carry_relays(&endpoint_id, 1);
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static bool image_pending_verify(void)
{
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    return running != nullptr && esp_ota_get_state_partition(running, &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

static bool ota_download_busy(void)
{
    if (!shelly_app_is_started()) {
        return false;
    }
    ShellyInputLock lock(SHELLY_INPUT_LOCK_TIMEOUT_MS);
    if (!lock.locked()) {
        // Matter is too busy to ask: refuse the restart, as for a download.
        ESP_LOGW(TAG, "Matter busy for %d ms: Wi-Fi mode refused", SHELLY_INPUT_LOCK_TIMEOUT_MS);
        return true;
    }
    chip::OTARequestorInterface *requestor = chip::GetRequestorInstance();
    return requestor != nullptr &&
           requestor->GetCurrentUpdateState() != chip::OTARequestorInterface::OTAUpdateStateEnum::kIdle;
}

static ui_state_t current_state(ui_input_t input, bool switch_active = false, uint8_t switch_channel = 0)
{
    ui_state_t st = {};
    st.channels = s_config.channels == 0 ? 1 : s_config.channels;
    st.switch_channel = switch_channel;
    st.maintenance = shelly_maintenance_active();
#if CONFIG_SHELLY_MAINTENANCE
    st.maintenance_available = true;
#endif
    st.started = shelly_app_is_started();
    // Only the holds that restart the device depend on these: skip the
    // otadata read and the lock for taps and the wall switch.
    st.pending_verify = (input == UI_INPUT_WIFI_MODE || input == UI_INPUT_FACTORY_RESET) && image_pending_verify();
    st.ota_busy = input == UI_INPUT_WIFI_MODE && !st.maintenance && ota_download_busy();
    st.change_pending = input == UI_INPUT_PAIRING && st.maintenance && shelly_change_pending(nullptr, nullptr);
    st.switch_role = s_config.switch_role;
    st.switch_mode = shelly_switch_mode_get_ch(switch_channel);
    st.switch_active = switch_active;
    return st;
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

static void open_pairing_work(intptr_t arg)
{
    chip::Server &server = chip::Server::GetInstance();
    chip::CommissioningWindowManager &mgr = server.GetCommissioningWindowManager();
    if (server.GetFailSafeContext().IsFailSafeArmed() || mgr.GetPASESession().HasValue()) {
        // Someone is pairing right now: leave their session alone.
        ESP_LOGW(TAG, "Pairing in progress, not reopening the window");
        status_led_flash_refused();
        return;
    }
    if (mgr.CommissioningWindowStatusForCluster() ==
        chip::app::Clusters::AdministratorCommissioning::CommissioningWindowStatusEnum::kEnhancedWindowOpen) {
        // A controller opened this window with its own pairing code (Share
        // device). Replacing it with a basic window would make the code the
        // controller shows stop working, so it stays as it is.
        ESP_LOGI(TAG, "Pairing window opened by a controller: left as it is");
        status_led_flash_accepted();
        return;
    }
    bool paired = server.GetFabricTable().FabricCount() > 0;
    uint16_t seconds = paired ? PAIRING_WINDOW_PAIRED_S : PAIRING_WINDOW_UNPAIRED_S;
    if (mgr.IsCommissioningWindowOpen()) {
        // Restart it with a full timeout.
        mgr.CloseCommissioningWindow();
    }
    CHIP_ERROR err = mgr.OpenBasicCommissioningWindow(chip::System::Clock::Seconds16(seconds),
                                                      chip::CommissioningWindowAdvertisement::kAllSupported);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Failed to open the pairing window: %" CHIP_ERROR_FORMAT, err.Format());
        status_led_flash_refused();
        return;
    }
    ESP_LOGI(TAG, "Pairing window open for %u s", seconds);
    status_led_flash_accepted();
}

static void run_toggle(shelly_ui_toggle_fn_t fn)
{
    if (fn == nullptr || !fn()) {
        status_led_flash_refused();
    }
}

static void run_toggle_to(shelly_ui_toggle_to_fn_t fn, bool on)
{
    if (fn == nullptr || !fn(on)) {
        status_led_flash_refused();
    }
}

static void carry_out(ui_outcome_t outcome, uint8_t ch)
{
    // Relay 2's functions on a 2-channel board; relay 1's otherwise.
    bool second = ch == 1 && s_config.channels > 1;
    shelly_ui_toggle_fn_t toggle = second ? s_config.toggle2 : s_config.toggle;
    shelly_ui_toggle_fn_t local_toggle = second ? s_config.local_toggle2 : s_config.local_toggle;
    shelly_ui_toggle_to_fn_t toggle_to = second ? s_config.toggle_to2 : s_config.toggle_to;
    shelly_ui_toggle_to_fn_t local_toggle_to = second ? s_config.local_toggle_to2 : s_config.local_toggle_to;
    switch (outcome) {
    case UI_IGNORE:
        break;

    case UI_VARIANT:
        if (s_config.switch_handler != nullptr) {
            s_config.switch_handler();
        }
        break;

    case UI_REFUSE:
        status_led_flash_refused();
        break;

    case UI_TOGGLE:
        run_toggle(toggle);
        break;

    case UI_LOCAL_TOGGLE:
        run_toggle(local_toggle);
        break;

    case UI_TURN_ON:
    case UI_TURN_OFF:
        run_toggle_to(toggle_to, outcome == UI_TURN_ON);
        break;

    case UI_LOCAL_TURN_ON:
    case UI_LOCAL_TURN_OFF:
        run_toggle_to(local_toggle_to, outcome == UI_LOCAL_TURN_ON);
        break;

    case UI_OPEN_PAIRING:
        if (chip::DeviceLayer::PlatformMgr().ScheduleWork(open_pairing_work, 0) != CHIP_NO_ERROR) {
            status_led_flash_refused();
        }
        break;

    case UI_ENTER_MAINTENANCE:
        if (shelly_maintenance_request() == ESP_OK) {
            status_led_flash_accepted();
            restart_after_flash();
        }
        status_led_flash_refused();
        break;

    case UI_EXIT_MAINTENANCE:
        ESP_LOGI(TAG, "Leaving maintenance mode");
        shelly_maintenance_exit_record();
        status_led_flash_accepted();
        restart_after_flash();
        break;

    case UI_FACTORY_RESET:
        status_led_flash_accepted();
        if (chip::DeviceLayer::PlatformMgr().ScheduleWork(factory_reset_work, 0) != CHIP_NO_ERROR) {
            ESP_LOGE(TAG, "Failed to schedule the factory reset");
            status_led_flash_refused();
        }
        break;

    case UI_DEFER_RESET:
        defer_factory_reset();
        break;

    case UI_CONFIRM_CHANGE:
        if (shelly_change_confirm()) {
            status_led_flash_accepted();
        } else {
            status_led_flash_refused();
        }
        break;
    }
}

static void on_button_action(button_action_t action, void *arg)
{
    ui_input_t input;
    switch (action) {
    case BUTTON_ACTION_TOGGLE:
        input = UI_INPUT_TAP;
        break;
    case BUTTON_ACTION_DOUBLE_TAP:
        input = UI_INPUT_TAP2;
        break;
    case BUTTON_ACTION_PAIRING:
        input = UI_INPUT_PAIRING;
        break;
    case BUTTON_ACTION_WIFI_MODE:
        input = UI_INPUT_WIFI_MODE;
        break;
    case BUTTON_ACTION_FACTORY_RESET:
        input = UI_INPUT_FACTORY_RESET;
        break;
    default:
        return;
    }
    ui_state_t st = current_state(input);
    carry_out(ui_decide(input, &st), ui_decide_channel(input, &st));
}

esp_err_t shelly_ui_init(const shelly_ui_config_t *config)
{
    s_config = *config;
    shelly_settings_init();
    // Diagnostics builds only; never in the maintenance boot, so the log a
    // failed pairing left stays readable on the page.
    if (!shelly_maintenance_active()) {
        shelly_diag_log_start();
    }
    return button_init(on_button_action, nullptr);
}

void shelly_ui_switch_changed_ch(int ch, bool active)
{
    ui_state_t st = current_state(UI_INPUT_SWITCH, active, (uint8_t)ch);
    carry_out(ui_decide(UI_INPUT_SWITCH, &st), ui_decide_channel(UI_INPUT_SWITCH, &st));
}

void shelly_ui_switch_changed(bool active)
{
    shelly_ui_switch_changed_ch(0, active);
}

ui_switch_role_t shelly_ui_switch_role(void)
{
    return s_config.switch_role;
}
