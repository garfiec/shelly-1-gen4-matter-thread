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

static void defer_factory_reset(void)
{
    ESP_LOGW(TAG, "Factory reset requested before Matter is up: restarting to do it");
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
static bool s_record_relay_on = false;

static uint32_t maint_check(const maint_record_t *r)
{
    return ~(r->magic ^ (r->relay_on * 0x9E3779B9u));
}

static void record_store(uint32_t magic)
{
    s_maint_record.magic = magic;
    s_maint_record.relay_on = relay_get() ? 1 : 0;
    s_maint_record.check = maint_check(&s_maint_record);
}

static void take_record(void)
{
    if (s_record_taken) {
        return;
    }
    s_record_taken = true;
    uint32_t magic = s_maint_record.magic;
    bool valid = (magic == MAINT_RECORD_MAGIC || magic == EXIT_RECORD_MAGIC) &&
                 s_maint_record.check == maint_check(&s_maint_record) && esp_reset_reason() == ESP_RST_SW;
    s_record_found = valid ? magic : 0;
    s_record_relay_on = s_maint_record.relay_on != 0;
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

bool shelly_maintenance_boot_pending(bool *relay_was_on)
{
    take_record();
    if (s_record_found != MAINT_RECORD_MAGIC || !MAINTENANCE_BUILT) {
        return false;
    }
    s_maintenance_active = true;
    if (relay_was_on != nullptr) {
        *relay_was_on = s_record_relay_on;
    }
    return true;
}

bool shelly_maintenance_active(void)
{
    return s_maintenance_active;
}

void shelly_maintenance_exit_record(void)
{
    record_store(EXIT_RECORD_MAGIC);
}

void shelly_maintenance_carry_relay(uint16_t endpoint_id)
{
    take_record();
    if (s_record_found != EXIT_RECORD_MAGIC || endpoint_id == 0) {
        return;
    }
    s_record_found = 0;
    bool on = s_record_relay_on;
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
    ESP_LOGI(TAG, "Back from Wi-Fi maintenance mode, where the relay was %s", on ? "on" : "off");
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

static ui_state_t current_state(ui_input_t input)
{
    ui_state_t st = {};
    st.maintenance = shelly_maintenance_active();
#if CONFIG_SHELLY_MAINTENANCE
    st.maintenance_available = true;
#endif
    st.started = shelly_app_is_started();
    // Only the holds that restart the device depend on these: skip the
    // otadata read and the lock for taps and the wall switch.
    st.pending_verify = (input == UI_INPUT_WIFI_MODE || input == UI_INPUT_FACTORY_RESET) && image_pending_verify();
    st.ota_busy = input == UI_INPUT_WIFI_MODE && !st.maintenance && ota_download_busy();
    st.switch_role = s_config.switch_role;
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

static void carry_out(ui_outcome_t outcome)
{
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
        run_toggle(s_config.toggle);
        break;

    case UI_LOCAL_TOGGLE:
        run_toggle(s_config.local_toggle);
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
    }
}

static void on_button_action(button_action_t action, void *arg)
{
    ui_input_t input;
    switch (action) {
    case BUTTON_ACTION_TOGGLE:
        input = UI_INPUT_TAP;
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
    carry_out(ui_decide(input, &st));
}

esp_err_t shelly_ui_init(const shelly_ui_config_t *config)
{
    s_config = *config;
    return button_init(on_button_action, nullptr);
}

void shelly_ui_switch_changed(void)
{
    ui_state_t st = current_state(UI_INPUT_SWITCH);
    carry_out(ui_decide(UI_INPUT_SWITCH, &st));
}
