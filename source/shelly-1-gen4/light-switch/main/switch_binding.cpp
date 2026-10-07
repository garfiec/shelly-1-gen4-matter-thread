//
// Copyright 2026 AUTOMATOUS.IO
//
// Detached relay support (the SW input drives a Matter binding instead of the
// local relay) contributed by Tomas McGuinness (https://github.com/tomasmcguinness).
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

// switch_binding.cpp

#include "switch_binding.h"

#include <app_priv.h>

#include <esp_log.h>
#include <esp_matter.h>

#include <shelly_app.h>
#include <switch_input.h>

// BooleanState StateValue is read-only / internally managed, so it must be
// updated via the cluster setter, not attribute::update().
#include <clusters/boolean_state/integration.h>

using namespace chip::app::Clusters;
using namespace esp_matter;

static const char *TAG = "switch_binding";

// Defined in app_main.cpp. The On/Off Light Switch endpoint whose
// bindings receive the Toggle command.
extern uint16_t switch_endpoint_id;

// Defined in app_main.cpp. The endpoint whose Boolean State StateValue
// attribute mirrors the wall toggle position. Zero until the endpoint is
// created; edges before then still send the toggle but skip the report.
extern uint16_t contact_sensor_endpoint_id;

bool switch_binding_read_state(void)
{
    // Map the raw GPIO level to the Matter StateValue (true = toggle on).
    return switch_input_get_level() == (SWITCH_ON_IS_HIGH ? true : false);
}

void switch_binding_report(void)
{
    // The endpoint may not exist yet during early boot edges.
    if (contact_sensor_endpoint_id == 0) {
        return;
    }

    bool state_value = switch_binding_read_state();

    // Runs outside the Matter task, so take the CHIP stack lock.
    // SetStateValue() updates the attribute and emits the report.
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    auto *cluster = chip::app::Clusters::BooleanState::FindClusterOnEndpoint(contact_sensor_endpoint_id);
    if (cluster == nullptr) {
        ESP_LOGE(TAG, "BooleanState cluster not found on endpoint %u", contact_sensor_endpoint_id);
        return;
    }
    cluster->SetStateValue(state_value);
    ESP_LOGI(TAG, "Switch position updated: %s", state_value ? "ON" : "OFF");
}

// Runs on the switch_input task after each debounced change of level. The
// device UI decides whether it acts: not before Matter is up, when the
// binding manager and the CHIP stack lock do not exist yet.
static void switch_binding_changed(bool level, void *arg)
{
    shelly_ui_switch_changed(switch_input_active(level));
}

void switch_binding_send(void)
{
    ESP_LOGI(TAG, "SW input changed. Sending Toggle command");
    client::request_handle_t req_handle;
    req_handle.type = esp_matter::client::INVOKE_CMD;
    req_handle.command_path.mClusterId = OnOff::Id;
    req_handle.command_path.mCommandId = OnOff::Commands::Toggle::Id;

    // Scoped so the stack lock releases before the report takes
    // its own below.
    {
        lock::ScopedChipStackLock lock(portMAX_DELAY);
        client::cluster_update(switch_endpoint_id, &req_handle);
    }

    ESP_LOGI(TAG, "Switch input toggled");

    switch_binding_report();
}

esp_err_t switch_binding_init(void)
{
    return switch_input_init(switch_binding_changed, nullptr);
}
