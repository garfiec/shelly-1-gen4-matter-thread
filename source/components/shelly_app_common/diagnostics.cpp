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

// diagnostics.cpp
//
// The hardware-fault registry, the GeneralDiagnostics provider, and the
// health task (task watchdog feed and OTA image confirmation).

#include "shelly_app.h"
#include "shelly_faults.h"

#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#include <shelly_setup_code.h>
#include <status_led.h>

#include <app/clusters/general-diagnostics-server/CodegenIntegration.h>
#include <app/clusters/ota-requestor/OTARequestorInterface.h>
#include <platform/ESP32/DiagnosticDataProviderImpl.h>
#include <platform/PlatformManager.h>

using namespace chip::DeviceLayer;
using namespace chip::app::Clusters;
using namespace esp_matter;

static const char *TAG = "diagnostics";

// ---------------------------------------------------------------------------
// Fault registry
// ---------------------------------------------------------------------------

static portMUX_TYPE s_fault_mux = portMUX_INITIALIZER_UNLOCKED;
// Sources currently raised (bit n = shelly_fault_src_t n).
static uint32_t s_fault_mask = 0;
// Sources last published to Matter. Only the Matter thread writes it.
static uint32_t s_published_mask = 0;
static volatile bool s_started = false;

static void fault_mask_to_general_faults(uint32_t mask, GeneralFaults<kMaxHardwareFaults> &faults)
{
    uint8_t list[kMaxHardwareFaults];
    size_t n = shelly_fault_mask_to_list(mask, list, kMaxHardwareFaults);
    for (size_t i = 0; i < n; i++) {
        // Cannot fail: n is bounded by the list capacity.
        (void)faults.add(list[i]);
    }
}

// Runs on the Matter thread with the stack lock held (ScheduleWork).
static void publish_faults_work(intptr_t arg)
{
    uint32_t mask;
    portENTER_CRITICAL(&s_fault_mux);
    mask = s_fault_mask;
    portEXIT_CRITICAL(&s_fault_mux);

    GeneralFaults<kMaxHardwareFaults> previous;
    GeneralFaults<kMaxHardwareFaults> current;
    fault_mask_to_general_faults(s_published_mask, previous);
    fault_mask_to_general_faults(mask, current);
    s_published_mask = mask;

    // Only the de-duplicated enum list is visible to controllers. A
    // source change that leaves the list unchanged emits nothing.
    if (previous.size() == current.size() &&
        memcmp(previous.data(), current.data(), current.size()) == 0) {
        return;
    }
    ESP_LOGW(TAG, "Hardware faults changed: %u -> %u active", (unsigned)previous.size(), (unsigned)current.size());
    GeneralDiagnostics::GlobalNotifyHardwareFaultsDetect(previous, current);
}

static void schedule_publish(void)
{
    CHIP_ERROR err = PlatformMgr().ScheduleWork(publish_faults_work, 0);
    if (err != CHIP_NO_ERROR) {
        // The next fault change schedules it again.
        ESP_LOGE(TAG, "Failed to schedule fault publish: %" CHIP_ERROR_FORMAT, err.Format());
    }
}

void shelly_fault_set(shelly_fault_src_t src, bool active)
{
    if (src >= SHELLY_FAULT_SRC_COUNT) {
        return;
    }
    uint32_t bit = 1u << src;
    uint32_t before, after;
    portENTER_CRITICAL(&s_fault_mux);
    before = s_fault_mask;
    s_fault_mask = active ? (s_fault_mask | bit) : (s_fault_mask & ~bit);
    after = s_fault_mask;
    portEXIT_CRITICAL(&s_fault_mux);

    if (before == after) {
        return;
    }
    if (active) {
        ESP_LOGE(TAG, "Fault raised: source %d", src);
    } else {
        ESP_LOGW(TAG, "Fault cleared: source %d", src);
    }
    // Another task can change the mask between the update above and the LED
    // write. Re-check after writing, so the last writer leaves the LED
    // matching the registry.
    bool shown;
    do {
        shown = after != 0;
        status_led_set_fault(shown);
        portENTER_CRITICAL(&s_fault_mux);
        after = s_fault_mask;
        portEXIT_CRITICAL(&s_fault_mux);
    } while ((after != 0) != shown);
    if (s_started) {
        schedule_publish();
    }
}

// ---------------------------------------------------------------------------
// GeneralDiagnostics provider
// ---------------------------------------------------------------------------

namespace {

// The ESP32 provider with two overrides. Everything else (RebootCount,
// UpTime, network interfaces, heap, Thread metrics) is inherited, so the
// attributes it backs keep working.
class ShellyDiagnosticDataProvider : public DiagnosticDataProviderImpl
{
public:
    // The ESP32 provider reports task-watchdog and panic resets as
    // Unspecified. Name them, so a watchdog reboot is visible remotely.
    CHIP_ERROR GetBootReason(BootReasonType &bootReason) override
    {
        switch (esp_reset_reason()) {
        case ESP_RST_TASK_WDT:
        case ESP_RST_INT_WDT:
            bootReason = BootReasonType::kSoftwareWatchdogReset;
            return CHIP_NO_ERROR;
        case ESP_RST_WDT:
            bootReason = BootReasonType::kHardwareWatchdogReset;
            return CHIP_NO_ERROR;
        case ESP_RST_PANIC:
            bootReason = BootReasonType::kSoftwareReset;
            return CHIP_NO_ERROR;
        default:
            return DiagnosticDataProviderImpl::GetBootReason(bootReason);
        }
    }

    // Reports what was last published, so a read always matches the
    // latest HardwareFaultChange event.
    CHIP_ERROR GetActiveHardwareFaults(GeneralFaults<kMaxHardwareFaults> &hardwareFaults) override
    {
        fault_mask_to_general_faults(s_published_mask, hardwareFaults);
        return CHIP_NO_ERROR;
    }
};

ShellyDiagnosticDataProvider s_diag_provider;

} // namespace

esp_err_t shelly_diagnostics_add_attributes(node_t *node)
{
    endpoint_t *root = endpoint::get(node, 0);
    cluster_t *cluster = root ? cluster::get(root, GeneralDiagnostics::Id) : nullptr;
    if (cluster == nullptr) {
        ESP_LOGE(TAG, "GeneralDiagnostics cluster not found on the root endpoint");
        return ESP_ERR_NOT_FOUND;
    }
    // Only these two optional attributes: create_optional_attributes()
    // would also add radio and network faults, which nothing here reports.
    if (cluster::general_diagnostics::attribute::create_boot_reason(cluster, 0) == nullptr ||
        cluster::general_diagnostics::attribute::create_active_hardware_faults(cluster, nullptr, 0, 0) == nullptr ||
        cluster::general_diagnostics::event::create_hardware_fault_change(cluster) == nullptr) {
        ESP_LOGE(TAG, "Failed to add GeneralDiagnostics attributes");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void shelly_diagnostics_install(void)
{
    SetDiagnosticDataProvider(&s_diag_provider);
}

// ---------------------------------------------------------------------------
// Health task: task watchdog and OTA confirmation
// ---------------------------------------------------------------------------

// Stop feeding the watchdog once the Matter thread has been unresponsive
// this long. The task watchdog (CONFIG_ESP_TASK_WDT_TIMEOUT_S) then panics
// and reboots.
#define HEALTH_STALL_LIMIT_US   (30LL * 1000 * 1000)
// Confirm a new OTA image after this long running healthy.
#define OTA_CONFIRM_AFTER_US    (60LL * 1000 * 1000)
// A gap this long between successful lock checks restarts the healthy
// period that an OTA image must complete before it is confirmed.
#define OTA_CONFIRM_MAX_GAP_US  (10LL * 1000 * 1000)
#define HEALTH_POLL_MS          1000
#define HEALTH_TASK_STACK_SIZE  3072

static void confirm_running_image(void)
{
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        // A web UI or UART install leaves otadata empty: there is nothing
        // to confirm and nothing to roll back to.
        ESP_LOGI(TAG, "No OTA state for the running image, nothing to confirm");
        return;
    }
    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "New firmware confirmed, rollback cancelled");
    } else {
        ESP_LOGW(TAG, "Could not confirm firmware: %s", esp_err_to_name(err));
    }
}

static void health_task(void *arg)
{
    esp_err_t wdt_err = esp_task_wdt_add(nullptr);
    if (wdt_err != ESP_OK) {
        ESP_LOGW(TAG, "Task watchdog not available: %s", esp_err_to_name(wdt_err));
    }

    const int64_t started_us = esp_timer_get_time();
    int64_t last_ok_us = started_us;
    // Start of the current unbroken run of responsive checks.
    int64_t healthy_since_us = started_us;
    bool confirmed = false;
    bool stall_logged = false;
    bool updating = false;

    while (true) {
        int64_t now = esp_timer_get_time();

        // The Matter thread holds the stack lock while it works and
        // releases it between events. Getting it at all proves the thread
        // is not wedged.
        bool responsive = false;
        if (PlatformMgr().TryLockChipStack()) {
            // While we have the lock anyway: is a Matter update running?
            // The LED says so (LED-16), so nobody unplugs the device.
            chip::OTARequestorInterface *requestor = chip::GetRequestorInstance();
            bool now_updating = false;
            if (requestor != nullptr) {
                auto st = requestor->GetCurrentUpdateState();
                now_updating = st == chip::OTARequestorInterface::OTAUpdateStateEnum::kDownloading ||
                               st == chip::OTARequestorInterface::OTAUpdateStateEnum::kApplying;
            }
            PlatformMgr().UnlockChipStack();
            if (now_updating != updating) {
                updating = now_updating;
                status_led_set_updating(updating);
            }
            if (now - last_ok_us > OTA_CONFIRM_MAX_GAP_US) {
                healthy_since_us = now;
            }
            last_ok_us = now;
            stall_logged = false;
            responsive = true;
        }

        bool healthy = (now - last_ok_us) < HEALTH_STALL_LIMIT_US;
        if (healthy) {
            if (wdt_err == ESP_OK) {
                esp_task_wdt_reset();
            }
        } else if (!stall_logged) {
            ESP_LOGE(TAG, "Matter thread unresponsive, letting the task watchdog reboot");
            stall_logged = true;
        }

        // Confirm on a check that just succeeded, after a full minute with no
        // long gap, so an image whose Matter thread wedges first rolls back.
        if (!confirmed && responsive && (now - healthy_since_us) >= OTA_CONFIRM_AFTER_US) {
            confirm_running_image();
            confirmed = true;
        }

        vTaskDelay(pdMS_TO_TICKS(HEALTH_POLL_MS));
    }
}

void shelly_app_started(void)
{
    s_started = true;
    // Publish anything raised during boot, before Matter could take it.
    schedule_publish();
    {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        shelly_led_refresh();
    }
    shelly_ui_matter_started();
    shelly_setup_code_log();

    if (xTaskCreate(health_task, "health", HEALTH_TASK_STACK_SIZE, nullptr, 3, nullptr) != pdPASS) {
        // Without the health task a new image is never confirmed and rolls
        // back on the next reset. That is the safe outcome.
        ESP_LOGE(TAG, "Failed to create health task");
    }
}

bool shelly_app_is_started(void)
{
    return s_started;
}
