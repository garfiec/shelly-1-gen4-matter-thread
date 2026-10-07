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

// contact_sensor.cpp

#include "contact_sensor.h"

#include <app_priv.h>

#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_core.h>

#include <shelly_app.h>
#include <switch_input.h>

// BooleanState StateValue is read-only / internally managed, so it must be
// updated via the cluster setter, not attribute::update().
#include <clusters/boolean_state/integration.h>

using namespace chip::app::Clusters;
using namespace esp_matter;

static const char *TAG = "contact_sensor";

// Defined in app_main.cpp. The endpoint whose Boolean State StateValue
// attribute we update when the door position changes. Zero until the
// endpoint is created; edges before then are ignored.
extern uint16_t contact_sensor_endpoint_id;

// The SW terminal, used here as the reed/limit switch input, is owned by
// the shared switch_input module (debounce, level compare, no internal
// pulls). This file maps its level to the Matter contact state.

// A change goes through the device UI, which decides what it does in each
// state (docs/BUTTON-AND-LED.md SW-04..06): once Matter is up, it calls
// contact_sensor_report() back (the switch_handler in app_main.cpp).
static void contact_sensor_changed(bool level, void *arg)
{
    shelly_ui_switch_changed(switch_input_active(level));
}

bool contact_sensor_read_state(void)
{
    // Map the raw GPIO level to the Matter StateValue (true = closed).
    bool is_open = (switch_input_get_level() == (CONTACT_OPEN_IS_HIGH ? true : false));
    return !is_open;
}

void contact_sensor_report(void)
{
    // The endpoint may not exist yet during early boot edges.
    if (contact_sensor_endpoint_id == 0) {
        return;
    }

    bool state_value = contact_sensor_read_state();

    // Runs outside the Matter task, so take the CHIP stack lock.
    // SetStateValue() updates the attribute and emits the report.
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    auto *cluster = chip::app::Clusters::BooleanState::FindClusterOnEndpoint(contact_sensor_endpoint_id);
    if (cluster == nullptr) {
        ESP_LOGE(TAG, "BooleanState cluster not found on endpoint %u", contact_sensor_endpoint_id);
        return;
    }
    cluster->SetStateValue(state_value);
    ESP_LOGI(TAG, "Contact state updated: %s", state_value ? "CLOSED" : "OPEN");
}

esp_err_t contact_sensor_init(void)
{
    return switch_input_init(contact_sensor_changed, nullptr);
}
