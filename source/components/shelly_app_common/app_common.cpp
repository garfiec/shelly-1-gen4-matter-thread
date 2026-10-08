//
// Copyright 2026 AUTOMATOUS.IO
// Portions derived from Espressif esp-matter examples,
// originally released into the public domain / CC0.
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

// app_common.cpp

#include "shelly_app.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>

#include <log_heap_numbers.h>
#include <relay.h>
#include <shelly_diag_log.h>
#include <status_led.h>

#include <esp_openthread.h>
#include <openthread/icmp6.h>

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <platform/PlatformManager.h>
#include <platform/ThreadStackManager.h>

using namespace chip::app::Clusters;
using namespace esp_matter;

static const char *TAG = "app_common";

constexpr auto k_timeout_seconds = 300;

esp_err_t shelly_nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // The partition cannot be used as is. Erasing it loses the Matter
        // fabrics, the Thread dataset and the energy total, but the device
        // comes back up ready to commission instead of boot-looping.
        ESP_LOGE(TAG, "NVS unusable (%s): ERASING nvs, the device must be commissioned again",
                 esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
    }
    return err;
}

void shelly_led_refresh(void)
{
    // Called on the Matter thread, or with the CHIP stack lock held.
    chip::Server &server = chip::Server::GetInstance();
    led_state_t state;
    if (server.GetCommissioningWindowManager().IsCommissioningWindowOpen()) {
        state = LED_STATE_BLE_ADVERTISING;
    } else if (server.GetFabricTable().FabricCount() == 0) {
        state = LED_STATE_UNPAIRED_IDLE;
    } else if (chip::DeviceLayer::ConnectivityMgr().IsThreadAttached()) {
        state = LED_STATE_THREAD_CONNECTED;
    } else {
        state = LED_STATE_THREAD_CONNECTING;
    }
    status_led_set_state(state);
}

// Answer ping on every Thread address, not only the mesh-internal ones.
// ESP-IDF's Thread netif glue sets "RLOC/ALOC only" when Thread starts
// (esp_openthread_netif_glue.c), so pings to the device's routable address
// go unanswered. Android's Matter pairing pings the device on that address
// in "Checking connectivity to Thread network" and gives up without a
// reply, even though the Matter session itself would work (found on a 1PM
// Gen4 behind an Aqara border router). Runs on the Matter thread, taking
// the Thread lock the way the Matter SDK does; set again on every Thread
// state change, after the glue has had its say.
static void thread_answer_all_pings(void)
{
    otInstance *ot = esp_openthread_get_instance();
    if (ot == nullptr) {
        return;
    }
    chip::DeviceLayer::ThreadStackMgr().LockThreadStack();
    if (otIcmp6GetEchoMode(ot) != OT_ICMP6_ECHO_HANDLER_ALL) {
        otIcmp6SetEchoMode(ot, OT_ICMP6_ECHO_HANDLER_ALL);
        ESP_LOGI(TAG, "Thread: answering ping on every address");
    }
    chip::DeviceLayer::ThreadStackMgr().UnlockThreadStack();
}

void shelly_app_event_cb(const chip::DeviceLayer::ChipDeviceEvent *event, intptr_t arg)
{
    shelly_diag_log_event(event->Type);
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "Interface IP Address changed");
        break;

    case chip::DeviceLayer::DeviceEventType::kThreadStateChange:
        ESP_LOGI(TAG, "Thread is %sattached", chip::DeviceLayer::ConnectivityMgr().IsThreadAttached() ? "" : "NOT ");
        thread_answer_all_pings();
        shelly_led_refresh();
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete");
        MEMORY_PROFILER_DUMP_HEAP_STAT("commissioning complete");
        shelly_led_refresh();
        break;

    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGI(TAG, "Commissioning failed, fail safe timer expired");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
        ESP_LOGI(TAG, "Commissioning session started");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStopped:
        ESP_LOGI(TAG, "Commissioning session stopped");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGI(TAG, "Commissioning window opened");
        MEMORY_PROFILER_DUMP_HEAP_STAT("commissioning window opened");
        // The pairing blink shows whenever a window is open, including one
        // opened on a paired device for a second ecosystem.
        shelly_led_refresh();
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
        ESP_LOGI(TAG, "Commissioning window closed");
        shelly_led_refresh();
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricRemoved: {
        ESP_LOGI(TAG, "Fabric removed successfully");
        if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
            chip::CommissioningWindowManager  &commissionMgr = chip::Server::GetInstance().GetCommissioningWindowManager();
            constexpr auto kTimeoutSeconds = chip::System::Clock::Seconds16(k_timeout_seconds);
            if (!commissionMgr.IsCommissioningWindowOpen()) {
                // After removing last fabric, re-open commissioning window via DNS-SD
                CHIP_ERROR err = commissionMgr.OpenBasicCommissioningWindow(kTimeoutSeconds,
                                                                            chip::CommissioningWindowAdvertisement::kDnssdOnly);
                if (err != CHIP_NO_ERROR) {
                    ESP_LOGE(TAG, "Failed to open commissioning window, err:%" CHIP_ERROR_FORMAT, err.Format());
                }
            }
        }
        shelly_led_refresh();
        break;
    }

    case chip::DeviceLayer::DeviceEventType::kFabricWillBeRemoved:
        ESP_LOGI(TAG, "Fabric will be removed");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricUpdated:
        ESP_LOGI(TAG, "Fabric is updated");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricCommitted:
        ESP_LOGI(TAG, "Fabric is committed");
        break;

    case chip::DeviceLayer::DeviceEventType::kBLEDeinitialized:
        ESP_LOGI(TAG, "BLE deinitialized and memory reclaimed");
        MEMORY_PROFILER_DUMP_HEAP_STAT("BLE deinitialized");
        break;

    default:
        break;
    }
}

// TriggerEffect only starts an effect; nothing sends STOP for it. The
// identify pattern runs for about as long as the effect would, on a timer,
// and FinishEffect or StopEffect end it.
static esp_timer_handle_t s_effect_timer = nullptr;

static void effect_timer_cb(void *arg)
{
    status_led_stop_identify();
}

static uint32_t effect_duration_ms(uint8_t effect_id)
{
    switch (static_cast<Identify::EffectIdentifierEnum>(effect_id)) {
    case Identify::EffectIdentifierEnum::kBreathe:
        return 15000;
    case Identify::EffectIdentifierEnum::kChannelChange:
        return 8000;
    default:
        return 2000;
    }
}

static void effect_timer_stop(void)
{
    if (s_effect_timer != nullptr) {
        esp_timer_stop(s_effect_timer);
    }
}

static void effect_start(uint8_t effect_id)
{
    if (s_effect_timer == nullptr) {
        const esp_timer_create_args_t args = {
            .callback = effect_timer_cb,
            .name = "identify_effect",
        };
        if (esp_timer_create(&args, &s_effect_timer) != ESP_OK) {
            // Without the timer the pattern would never end: skip the effect.
            ESP_LOGE(TAG, "Failed to create the identify effect timer");
            s_effect_timer = nullptr;
            return;
        }
    }
    status_led_start_identify();
    esp_timer_stop(s_effect_timer);
    esp_timer_start_once(s_effect_timer, (uint64_t)effect_duration_ms(effect_id) * 1000);
}

// This callback is invoked when clients interact with the Identify Cluster.
// Triggered by controllers (Apple Home, HA, Google Home) when a user wants
// to physically identify which device is which. The LED enters an asymmetric
// blink pattern for the duration of the Identify command, then restores.
esp_err_t shelly_app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id, uint8_t effect_id,
                                       uint8_t effect_variant, void *priv_data)
{
    ESP_LOGI(TAG, "Identification callback: type: %u, effect: %u, variant: %u", type, effect_id, effect_variant);

    switch (type) {
    case identification::callback_type_t::START:
        ESP_LOGI(TAG, "Identify START - blinking status LED");
        effect_timer_stop();
        status_led_start_identify();
        break;

    case identification::callback_type_t::STOP:
        ESP_LOGI(TAG, "Identify STOP - restoring status LED");
        effect_timer_stop();
        status_led_stop_identify();
        break;

    case identification::callback_type_t::EFFECT:
        // TriggerEffect command - effect_id specifies which effect.
        // All effects map to the basic identify blink, for a time.
        if (effect_id == static_cast<uint8_t>(Identify::EffectIdentifierEnum::kFinishEffect) ||
            effect_id == static_cast<uint8_t>(Identify::EffectIdentifierEnum::kStopEffect)) {
            ESP_LOGI(TAG, "Identify EFFECT %u - ending the blink", effect_id);
            effect_timer_stop();
            status_led_stop_identify();
        } else {
            ESP_LOGI(TAG, "Identify EFFECT %u variant %u - using default blink", effect_id, effect_variant);
            effect_start(effect_id);
        }
        break;

    default:
        ESP_LOGW(TAG, "Unknown identification callback type: %u", type);
        break;
    }

    return ESP_OK;
}

// attribute::update() takes the CHIP stack lock again inside a
// ShellyInputLock: that nests only with lock tracking (ALREADY_TAKEN).
#if !CHIP_STACK_LOCK_TRACKING_ENABLED
#error "ShellyInputLock needs CHIP_STACK_LOCK_TRACKING_ENABLED"
#endif

ShellyInputLock::ShellyInputLock(uint32_t timeout_ms)
{
    chip::DeviceLayer::PlatformManager &platform = chip::DeviceLayer::PlatformMgr();
    if (platform.IsChipStackLockedByCurrentThread()) {
        m_locked = true;
        return;
    }
    const TickType_t start = xTaskGetTickCount();
    const TickType_t limit = pdMS_TO_TICKS(timeout_ms);
    while (!platform.TryLockChipStack()) {
        if (xTaskGetTickCount() - start >= limit) {
            return;
        }
        vTaskDelay(1);
    }
    m_locked = true;
    m_owned = true;
}

ShellyInputLock::~ShellyInputLock()
{
    if (m_owned) {
        chip::DeviceLayer::PlatformMgr().UnlockChipStack();
    }
}

static shelly_local_toggle_hook_t s_local_toggle_hook = nullptr;
static shelly_local_toggle_done_t s_local_toggle_done = nullptr;

void shelly_onoff_set_local_toggle_hook(shelly_local_toggle_hook_t hook, shelly_local_toggle_done_t done)
{
    s_local_toggle_hook = hook;
    s_local_toggle_done = done;
}

// Relay endpoints after the first (index = channel - 1). Every other
// endpoint is channel 0, so single-relay variants register nothing.
static uint16_t s_extra_relay_endpoints[RELAY_CHANNELS > 1 ? RELAY_CHANNELS - 1 : 1];

void shelly_relay_endpoint_register(int ch, uint16_t endpoint_id)
{
    if (ch >= 1 && ch < RELAY_CHANNELS) {
        s_extra_relay_endpoints[ch - 1] = endpoint_id;
    }
}

int shelly_relay_endpoint_channel(uint16_t endpoint_id)
{
    for (int ch = 1; ch < RELAY_CHANNELS; ch++) {
        if (endpoint_id != 0 && s_extra_relay_endpoints[ch - 1] == endpoint_id) {
            return ch;
        }
    }
    return 0;
}

// A local toggle, or with want >= 0 a local set to that value. Shared so a
// set runs exactly the toggle's lock and hooks. A set to the value already
// there writes nothing, but still runs the hooks: the user's choice counts
// (on the 1PM, a switch turned off cancels an overvoltage restore).
static bool onoff_local_change(uint16_t endpoint_id, int want)
{
    if (!shelly_app_is_started() || endpoint_id == 0) {
        return false;
    }
    // Read, hook and write under one lock, so a Matter command cannot
    // change the value between the read and the write.
    ShellyInputLock lock(SHELLY_INPUT_LOCK_TIMEOUT_MS);
    if (!lock.locked()) {
        ESP_LOGW(TAG, "Matter busy for %d ms: toggle refused", SHELLY_INPUT_LOCK_TIMEOUT_MS);
        return false;
    }
    attribute_t *attribute = attribute::get(endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id);
    if (attribute == nullptr) {
        return false;
    }
    esp_matter_attr_val_t val = esp_matter_invalid(nullptr);
    if (attribute::get_val(attribute, &val) != ESP_OK) {
        return false;
    }
    bool target = want >= 0 ? want != 0 : !val.val.b;
    if (s_local_toggle_hook != nullptr) {
        s_local_toggle_hook(target);
    }
    // Nothing to write: report whether the relay really is there, so a
    // hook that cleared the 1PM latch re-arms it if the relay is off.
    bool accepted = relay_get_ch(shelly_relay_endpoint_channel(endpoint_id)) == target;
    if (val.val.b != target) {
        val.val.b = target;
        // The PRE_UPDATE callback drives the relay; its refusal (thermal
        // lockout, protection) comes back here and leaves the attribute as is.
        accepted = attribute::update(endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &val) == ESP_OK;
    }
    if (s_local_toggle_done != nullptr) {
        s_local_toggle_done(accepted);
    }
    return accepted;
}

bool shelly_onoff_toggle(uint16_t endpoint_id)
{
    return onoff_local_change(endpoint_id, -1);
}

bool shelly_onoff_toggle_to(uint16_t endpoint_id, bool on)
{
    return onoff_local_change(endpoint_id, on ? 1 : 0);
}

static bool local_relay_change(int ch, int want)
{
    bool on = relay_get_ch(ch);
    bool target = want >= 0 ? want != 0 : !on;
    if (s_local_toggle_hook != nullptr) {
        s_local_toggle_hook(target);
    }
    bool accepted = on == target || relay_set_ch(ch, target) == ESP_OK;
    if (s_local_toggle_done != nullptr) {
        s_local_toggle_done(accepted);
    }
    return accepted;
}

bool shelly_local_relay_toggle_ch(int ch)
{
    return local_relay_change(ch, -1);
}

bool shelly_local_relay_toggle_to_ch(int ch, bool on)
{
    return local_relay_change(ch, on ? 1 : 0);
}

bool shelly_local_relay_toggle(void)
{
    return local_relay_change(0, -1);
}

bool shelly_local_relay_toggle_to(bool on)
{
    return local_relay_change(0, on ? 1 : 0);
}

void shelly_onoff_set(uint16_t endpoint_id, bool on)
{
    if (!shelly_app_is_started() || endpoint_id == 0) {
        return;
    }
    esp_matter_attr_val_t val = esp_matter_bool(on);
    attribute::update(endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &val);
}

static void onoff_set_work(intptr_t arg)
{
    uint16_t endpoint_id = (uint16_t)(arg >> 1);
    bool on = (arg & 1) != 0;
    esp_matter_attr_val_t val = esp_matter_bool(on);
    attribute::update(endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &val);
}

void shelly_onoff_set_async(uint16_t endpoint_id, bool on)
{
    if (!shelly_app_is_started() || endpoint_id == 0) {
        return;
    }
    intptr_t arg = ((intptr_t)endpoint_id << 1) | (on ? 1 : 0);
    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(onoff_set_work, arg);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Failed to schedule OnOff update: %" CHIP_ERROR_FORMAT, err.Format());
    }
}
