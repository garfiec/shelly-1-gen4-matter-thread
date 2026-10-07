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

#if CONFIG_SHELLY_MAINTENANCE
#include <maintenance.h>
#endif

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
    lock::ScopedChipStackLock lock(portMAX_DELAY);
    chip::OTARequestorInterface *requestor = chip::GetRequestorInstance();
    return requestor != nullptr &&
           requestor->GetCurrentUpdateState() != chip::OTARequestorInterface::OTAUpdateStateEnum::kIdle;
}

static ui_state_t current_state(ui_input_t input)
{
    ui_state_t st = {};
#if CONFIG_SHELLY_MAINTENANCE
    st.maintenance = shelly_maintenance_active();
    st.maintenance_available = true;
#endif
    st.started = shelly_app_is_started();
    st.pending_verify = image_pending_verify();
    // Only Wi-Fi mode depends on it; skip the lock for everything else.
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
    if (server.GetFailSafeContext().IsFailSafeArmed()) {
        // Someone is pairing right now: leave their session alone.
        ESP_LOGW(TAG, "Pairing in progress, not reopening the window");
        status_led_flash_refused();
        return;
    }
    chip::CommissioningWindowManager &mgr = server.GetCommissioningWindowManager();
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
    case UI_VARIANT:
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
#if CONFIG_SHELLY_MAINTENANCE
        if (shelly_maintenance_request() == ESP_OK) {
            status_led_flash_accepted();
            restart_after_flash();
        }
#endif
        status_led_flash_refused();
        break;

    case UI_EXIT_MAINTENANCE:
        ESP_LOGI(TAG, "Leaving maintenance mode");
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
