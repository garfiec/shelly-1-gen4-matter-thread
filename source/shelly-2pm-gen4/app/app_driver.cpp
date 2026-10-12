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

// app_driver.cpp

#include <esp_log.h>

#include <esp_matter.h>
#include <app_priv.h>
#include <common_macros.h>

#include <relay.h>
#include <shelly_app.h>

using namespace chip::app::Clusters;
using namespace esp_matter;

extern uint16_t relay_endpoint_id;
extern uint16_t relay2_endpoint_id;

// Which relay an endpoint drives: 0 (relay 1), 1 (relay 2), -1 none.
static int relay_channel(uint16_t endpoint_id)
{
    if (endpoint_id == relay_endpoint_id) {
        return 0;
    }
    if (endpoint_id == relay2_endpoint_id) {
        return 1;
    }
    return -1;
}

esp_err_t app_driver_attribute_update(app_driver_handle_t driver_handle, uint16_t endpoint_id, uint32_t cluster_id,
                                      uint32_t attribute_id, esp_matter_attr_val_t *val)
{
    esp_err_t err = ESP_OK;
    int ch = relay_channel(endpoint_id);
    if (ch >= 0) {
        if (cluster_id == OnOff::Id) {
            if (attribute_id == OnOff::Attributes::OnOff::Id) {
                err = relay_set_ch(ch, val->val.b);
            }
        }
    }
    return err;
}

esp_err_t app_driver_relay_set_defaults(uint16_t endpoint_id)
{
    esp_err_t err = ESP_OK;
    esp_matter_attr_val_t val = esp_matter_invalid(NULL);

    attribute_t *attribute = attribute::get(endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id);
    attribute::get_val(attribute, &val);
    int ch = relay_channel(endpoint_id);
    if (ch < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    err = relay_set_ch(ch, val.val.b);
    if (err != ESP_OK && val.val.b) {
        // Turn-on refused (thermal lockout): report the relay as off.
        shelly_onoff_set(endpoint_id, false);
    }

    return err;
}