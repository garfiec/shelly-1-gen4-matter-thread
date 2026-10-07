//
// Copyright 2026 AUTOMATOUS.IO
// Portions derived from Espressif esp-matter examples,
// originally released into the public domain / CC0.
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

// app_main.cpp

#include <esp_err.h>
#include <esp_log.h>

#include <esp_matter.h>
#include <esp_matter_console.h>
#include <esp_matter_ota.h>

#include <common_macros.h>
#include <log_heap_numbers.h>

#include <app_priv.h>
#include <shelly_app.h>
#include <shelly_setup_code.h>
#if CONFIG_SHELLY_MAINTENANCE
#include <maintenance.h>
#endif
#include <status_led.h>
#include <thermal.h>
#include <switch_binding.h>
#include <relay.h>

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include <platform/ESP32/OpenthreadLauncher.h>
#endif

#ifdef CONFIG_ENABLE_SET_CERT_DECLARATION_API
#include <esp_matter_providers.h>
#include <lib/support/Span.h>
#ifdef CONFIG_SEC_CERT_DAC_PROVIDER
#include <platform/ESP32/ESP32SecureCertDACProvider.h>
#elif defined(CONFIG_FACTORY_PARTITION_DAC_PROVIDER)
#include <platform/ESP32/ESP32FactoryDataProvider.h>
#endif
using namespace chip::DeviceLayer;
#endif

static const char *TAG = "app_main";
uint16_t light_endpoint_id = 0;
uint16_t switch_endpoint_id = 0;
uint16_t temperature_endpoint_id = 0;
uint16_t contact_sensor_endpoint_id = 0;

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;

#ifdef CONFIG_ENABLE_SET_CERT_DECLARATION_API
extern const uint8_t cd_start[] asm("_binary_certification_declaration_der_start");
extern const uint8_t cd_end[] asm("_binary_certification_declaration_der_end");

const chip::ByteSpan cdSpan(cd_start, static_cast<size_t>(cd_end - cd_start));
#endif // CONFIG_ENABLE_SET_CERT_DECLARATION_API

#if CONFIG_ENABLE_ENCRYPTED_OTA
extern const char decryption_key_start[] asm("_binary_esp_image_encryption_key_pem_start");
extern const char decryption_key_end[] asm("_binary_esp_image_encryption_key_pem_end");

static const char *s_decryption_key = decryption_key_start;
static const uint16_t s_decryption_key_len = decryption_key_end - decryption_key_start;
#endif // CONFIG_ENABLE_ENCRYPTED_OTA

// This callback is called for every attribute update. The callback implementation shall
// handle the desired attributes and return an appropriate error code. If the attribute
// is not of your interest, please do not return an error code and strictly return ESP_OK.
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                                         uint32_t attribute_id, esp_matter_attr_val_t *val, void *priv_data)
{
    esp_err_t err = ESP_OK;

    if (type == PRE_UPDATE) {
        /* Driver update */
        app_driver_handle_t driver_handle = (app_driver_handle_t)priv_data;
        err = app_driver_attribute_update(driver_handle, endpoint_id, cluster_id, attribute_id, val);
    }

    return err;
}

// A button tap toggles the local relay through the OnOff attribute, so
// Matter sees every change. The SW terminal drives the bound lights
// (switch_binding), not the relay. What each input does in each state is
// the device UI: docs/BUTTON-AND-LED.md.
static bool app_toggle(void)
{
    return shelly_onoff_toggle(light_endpoint_id);
}

extern "C" void app_main()
{
    esp_err_t err = ESP_OK;

    // Initialize the ESP NVS layer (erased and re-initialized if unusable)
    shelly_nvs_init();

    // Initialize status LED
    status_led_init();

    // Wi-Fi maintenance mode (the button held 5 s) is a boot of its own:
    // Matter and Thread are never started. See docs/BUTTON-AND-LED.md.
#if CONFIG_SHELLY_MAINTENANCE
    bool relay_was_on = false;
    if (shelly_maintenance_boot_pending(&relay_was_on)) {
        relay_init();
        thermal_init();
        shelly_ui_config_t maintenance_ui = {
            .toggle = app_toggle,
            .local_toggle = shelly_local_relay_toggle,
            .switch_role = UI_SWITCH_BINDING,
            .switch_handler = switch_binding_send,
        };
        shelly_ui_init(&maintenance_ui);
        shelly_maintenance_run(relay_was_on, shelly_local_relay_toggle);
    }
#endif

    MEMORY_PROFILER_DUMP_HEAP_STAT("Bootup");

    // Initialize subsystems. Order matters for safety:
    // relay first so the GPIO is in known-safe state before anything else.
    // thermal second so overtemp protection is active as early as possible.
    // switch_binding and button last for user-facing inputs.
    relay_init();
    thermal_init();
    switch_binding_init();
    shelly_ui_config_t ui_config = {
        .toggle = app_toggle,
        .local_toggle = shelly_local_relay_toggle,
        .switch_role = UI_SWITCH_BINDING,
        .switch_handler = switch_binding_send,
    };
    shelly_ui_init(&ui_config);

    app_driver_init();

    // Create a Matter node and add the mandatory Root Node device type on endpoint 0
    node::config_t node_config;

    // node handle can be used to add/modify other endpoints.
    node_t *node = node::create(&node_config, app_attribute_update_cb, shelly_app_identification_cb);
    ABORT_APP_ON_FAILURE(node != nullptr, ESP_LOGE(TAG, "Failed to create Matter node"));

    MEMORY_PROFILER_DUMP_HEAP_STAT("node created");

    // Create the On/Off Light device type. This is connected to the Shelly's Relay
    on_off_light::config_t light_config;
    light_config.on_off.on_off = DEFAULT_POWER;

    // endpoint handles can be used to add/modify clusters.
    endpoint_t *on_off_endpoint = on_off_light::create(node, &light_config, ENDPOINT_FLAG_NONE, nullptr);
    ABORT_APP_ON_FAILURE(on_off_endpoint != nullptr, ESP_LOGE(TAG, "Failed to create on_off_endpoint"));

    light_endpoint_id = endpoint::get_id(on_off_endpoint);
    ESP_LOGI(TAG, "Light created with endpoint_id %d", light_endpoint_id);

    // Create the On/Off Light Switch device type. This is connected to the Shelly's SW Input
    on_off_light_switch::config_t switch_config;
    endpoint_t *switch_endpoint = on_off_light_switch::create(node, &switch_config, ENDPOINT_FLAG_NONE, nullptr);
    ABORT_APP_ON_FAILURE(switch_endpoint != nullptr, ESP_LOGE(TAG, "Failed to create switch_endpoint"));

    switch_endpoint_id = endpoint::get_id(switch_endpoint);
    ESP_LOGI(TAG, "Switch created with endpoint_id %d", switch_endpoint_id);

    // Temperature Sensor endpoint exposes the die temperature the thermal
    // monitor already polls. Starts null until the first reading; min/max
    // match the 20-100°C range the sensor is configured for in thermal.cpp.
    temperature_sensor::config_t temp_sensor_config;
    temp_sensor_config.temperature_measurement.measured_value = nullable<int16_t>();
    temp_sensor_config.temperature_measurement.min_measured_value = 2000;
    temp_sensor_config.temperature_measurement.max_measured_value = 10000;

    endpoint_t *temp_endpoint = temperature_sensor::create(node, &temp_sensor_config, ENDPOINT_FLAG_NONE, nullptr);
    ABORT_APP_ON_FAILURE(temp_endpoint != nullptr, ESP_LOGE(TAG, "Failed to create temperature_sensor endpoint"));

    temperature_endpoint_id = endpoint::get_id(temp_endpoint);
    ESP_LOGI(TAG, "Temperature sensor created with endpoint_id %d", temperature_endpoint_id);

    thermal_set_endpoints(light_endpoint_id, temperature_endpoint_id);

    // Boot reason and hardware faults on the root endpoint's
    // GeneralDiagnostics. Attributes only; no endpoint or cluster is added.
    shelly_diagnostics_add_attributes(node);

    // Contact Sensor mirrors the wall toggle position on GPIO10, which
    // detached mode otherwise hides from controllers. Created last so
    // existing endpoint ids stay stable for devices upgrading over OTA.
    // Seed the initial state from the current position.
    contact_sensor::config_t contact_config;
    contact_config.boolean_state.state_value = switch_binding_read_state();
    endpoint_t *contact_endpoint = contact_sensor::create(node, &contact_config, ENDPOINT_FLAG_NONE, nullptr);
    ABORT_APP_ON_FAILURE(contact_endpoint != nullptr, ESP_LOGE(TAG, "Failed to create contact_sensor endpoint"));

    contact_sensor_endpoint_id = endpoint::get_id(contact_endpoint);
    ESP_LOGI(TAG, "Contact sensor created with endpoint_id %d", contact_sensor_endpoint_id);

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD && CHIP_DEVICE_CONFIG_ENABLE_WIFI_STATION
    // Enable secondary network interface
    secondary_network_interface::config_t secondary_network_interface_config;
    endpoint = endpoint::secondary_network_interface::create(node, &secondary_network_interface_config, ENDPOINT_FLAG_NONE, nullptr);
    ABORT_APP_ON_FAILURE(endpoint != nullptr, ESP_LOGE(TAG, "Failed to create secondary network interface endpoint"));
#endif

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    // Set OpenThread platform config
    esp_openthread_platform_config_t config = {
        .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
        .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
    };
    set_openthread_platform_config(&config);
#endif

#ifdef CONFIG_ENABLE_SET_CERT_DECLARATION_API
    auto * dac_provider = get_dac_provider();
#ifdef CONFIG_SEC_CERT_DAC_PROVIDER
    static_cast<ESP32SecureCertDACProvider *>(dac_provider)->SetCertificationDeclaration(cdSpan);
#elif defined(CONFIG_FACTORY_PARTITION_DAC_PROVIDER)
    static_cast<ESP32FactoryDataProvider *>(dac_provider)->SetCertificationDeclaration(cdSpan);
#endif
#endif // CONFIG_ENABLE_SET_CERT_DECLARATION_API

    // Back from Wi-Fi maintenance mode: its relay state is the previous
    // state StartUpOnOff applies to (docs/BUTTON-AND-LED.md P-03).
    shelly_maintenance_carry_relay(light_endpoint_id);

    // The diagnostics provider must be in place before Matter starts: the
    // GeneralDiagnostics boot event reads it at startup.
    shelly_diagnostics_install();
    // The device's own setup code (docs/COMMISSIONING.md): also before start.
    shelly_setup_code_install();

    // Matter start
    err = esp_matter::start(shelly_app_event_cb);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to start Matter, err:%d", err));

    MEMORY_PROFILER_DUMP_HEAP_STAT("matter started");

    // Publish boot-time faults, start the watchdog feed and OTA confirmation.
    shelly_app_started();

    // Starting driver with default values
    app_driver_light_set_defaults(light_endpoint_id);

    // Publish the actual switch position; the code-driven Boolean State
    // cluster needs an explicit setter call (the config seed doesn't reach it).
    switch_binding_report();

#if CONFIG_ENABLE_ENCRYPTED_OTA
    err = esp_matter_ota_requestor_encrypted_init(s_decryption_key, s_decryption_key_len);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to initialized the encrypted OTA, err: %d", err));
#endif // CONFIG_ENABLE_ENCRYPTED_OTA

#if CONFIG_ENABLE_CHIP_SHELL
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::wifi_register_commands();
    esp_matter::console::factoryreset_register_commands();
    esp_matter::console::attribute_register_commands();
#if CONFIG_OPENTHREAD_CLI
    esp_matter::console::otcli_register_commands();
#endif
    esp_matter::console::init();
#endif

    while (true) {
        MEMORY_PROFILER_DUMP_HEAP_STAT("Idle");
        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }
}
