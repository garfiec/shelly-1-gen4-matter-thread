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

// shelly_switch_binding.cpp
//
// The Light Switch type: SW drives the devices bound to the On/Off Light
// Switch endpoint, and a Contact Sensor endpoint mirrors its position.

#include "switch_binding.h"


#include <inttypes.h>

#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_client.h>

#include <shelly_app.h>
#include <switch_input.h>

// BooleanState StateValue is read-only / internally managed, so it must be
// updated via the cluster setter, not attribute::update().
#include <clusters/boolean_state/integration.h>

using namespace chip::app::Clusters;
using namespace esp_matter;

static const char *TAG = "switch_binding";

// SWITCH_ON_IS_HIGH: maps the SW level to the reported StateValue.
// SW closed reads HIGH; 0 reports a closed toggle as "open" on purpose,
// since HA renders contact sensors inverted (entity on = StateValue false).
// Confirmed on hardware: bridged SW shows ON in HA at 0.
#ifndef SWITCH_ON_IS_HIGH
#define SWITCH_ON_IS_HIGH 0
#endif

// The On/Off Light Switch endpoint whose bindings receive the Toggle
// command, and the endpoint whose Boolean State StateValue attribute
// mirrors the wall toggle position. Zero until created; edges before then
// still send the toggle but skip the report.
static uint16_t switch_endpoint_id = 0;
static uint16_t contact_sensor_endpoint_id = 0;

void switch_binding_set_endpoints(uint16_t switch_endpoint, uint16_t contact_endpoint)
{
    switch_endpoint_id = switch_endpoint;
    contact_sensor_endpoint_id = contact_endpoint;
}

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

// Invoked by the binding manager once the bound peer's CASE session is ready.
// This is where the queued command is actually put on the wire.
static void send_command_success_callback(void *context, const ConcreteCommandPath &command_path,
                                          const chip::app::StatusIB &status, TLVReader *response_data)
{
    ESP_LOGI(TAG, "Bound command sent successfully");
}

static void send_command_failure_callback(void *context, CHIP_ERROR error)
{
    ESP_LOGE(TAG, "Bound command send failed: %" CHIP_ERROR_FORMAT, error.Format());
}

// Unicast binding callback: build and send the invoke command to the bound peer.
static void app_driver_client_callback(client::peer_device_t *peer_device, client::request_handle_t *req_handle,
                                       void *priv_data)
{
    if (req_handle->type != esp_matter::client::INVOKE_CMD) {
        return;
    }
    if (req_handle->command_path.mClusterId != OnOff::Id) {
        ESP_LOGE(TAG, "Unsupported cluster 0x%08" PRIx32 " for bound command", req_handle->command_path.mClusterId);
        return;
    }
    // OnOff Toggle/On/Off take no fields, so the command data is an empty object.
    const char *command_data_str = "{}";
    client::interaction::invoke::send_request(NULL, peer_device, req_handle->command_path, command_data_str,
                                              send_command_success_callback, send_command_failure_callback,
                                              chip::NullOptional);
}

// Group binding callback: send the command to the bound group.
static void app_driver_client_group_callback(uint8_t fabric_index, client::request_handle_t *req_handle,
                                             void *priv_data)
{
    if (req_handle->type != esp_matter::client::INVOKE_CMD) {
        return;
    }
    if (req_handle->command_path.mClusterId != OnOff::Id) {
        ESP_LOGE(TAG, "Unsupported cluster 0x%08" PRIx32 " for bound group command", req_handle->command_path.mClusterId);
        return;
    }
    const char *command_data_str = "{}";
    client::interaction::invoke::send_group_request(fabric_index, req_handle->command_path, command_data_str);
}

void switch_binding_client_init(void)
{
    // Register the binding dispatch callbacks. Without these, client::cluster_update()
    // resolves the bound peer and opens a CASE session but never emits the command.
    client::set_request_callback(app_driver_client_callback, app_driver_client_group_callback, NULL);
}

esp_err_t switch_binding_init(void)
{
    return switch_input_init(switch_binding_changed, nullptr);
}
