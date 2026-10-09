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
#include <metering.h>
#include <protection.h>
#include <shelly_hw_snapshot.h>
#include <status_led.h>
#include <thermal.h>
#include <switch_input.h>
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
// The On/Off Light endpoints that drive relay 1 (O1) and relay 2 (O2).
uint16_t relay_endpoint_id = 0;
uint16_t relay2_endpoint_id = 0;
// The Temperature Sensor endpoint that reports the board temperature.
uint16_t temperature_endpoint_id = 0;

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

// The button and the wall switch toggle the relay through the OnOff
// attribute, so Matter sees every change. What each input does in each
// state is the device UI: docs/BUTTON-AND-LED.md.
static bool app_toggle(void)
{
    return shelly_onoff_toggle(relay_endpoint_id);
}

static bool app_toggle_to(bool on)
{
    return shelly_onoff_toggle_to(relay_endpoint_id, on);
}

static bool app_toggle2(void)
{
    return shelly_onoff_toggle(relay2_endpoint_id);
}

static bool app_toggle_to2(bool on)
{
    return shelly_onoff_toggle_to(relay2_endpoint_id, on);
}

static bool local_toggle2(void)
{
    return shelly_local_relay_toggle_ch(1);
}

static bool local_toggle_to2(bool on)
{
    return shelly_local_relay_toggle_to_ch(1, on);
}

// SW1 drives relay 1 and SW2 relay 2, each in its own wall switch mode.
static void app_switch_changed(int ch, bool level, void *arg)
{
    shelly_ui_switch_changed_ch(ch, switch_input_active(level));
}

static const shelly_ui_config_t UI_CONFIG = {
    .toggle = app_toggle,
    .local_toggle = shelly_local_relay_toggle,
    .toggle_to = app_toggle_to,
    .local_toggle_to = shelly_local_relay_toggle_to,
    .switch_role = UI_SWITCH_TOGGLE,
    .switch_handler = nullptr,
    .channels = 2,
    .toggle2 = app_toggle2,
    .local_toggle2 = local_toggle2,
    .toggle_to2 = app_toggle_to2,
    .local_toggle_to2 = local_toggle_to2,
};

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
    uint32_t relays_were_on = 0;
    if (shelly_maintenance_boot_pending_mask(&relays_were_on)) {
        relay_init();
        thermal_init();
        protection_init();
        protection_attach_local_inputs();
        // The meter feeds protection and the page's readings and calibration.
        // Skipped after a Wi-Fi mode boot that died with it running, so Wi-Fi
        // mode stays usable; overcurrent and overvoltage are then off here,
        // so the relays start off rather than restoring a load unprotected.
        bool meter_on = shelly_maintenance_meter_allowed();
        if (meter_on) {
            metering_init();
        }
        switch_input_init_ch(app_switch_changed, nullptr);
        shelly_ui_init(&UI_CONFIG);
        // The relay gate refuses a latched trip anyway; this keeps the
        // restore from even trying.
        for (int ch = 0; ch < RELAY_CHANNELS; ch++) {
            if (!meter_on || protection_is_latched_ch(ch)) {
                relays_were_on &= ~(1u << ch);
            }
        }
        shelly_maintenance_run_ch(relays_were_on, shelly_local_relay_toggle_ch);
    }
#endif

    MEMORY_PROFILER_DUMP_HEAP_STAT("Bootup");

    // Initialize subsystems. Order matters for safety:
    // relay first so the GPIOs are in known-safe state before anything else.
    // thermal and electrical protection next, so their gates hold before
    // anything can turn a relay on. Metering starts the meter task that
    // feeds protection, and decides (up to 3s) whether this board is on AC
    // mains and advertises the meter.
    // switch_input and button last for user-facing inputs.
    relay_init();
    thermal_init();
    protection_init();
    metering_init();
    switch_input_init_ch(app_switch_changed, nullptr);
    shelly_ui_init(&UI_CONFIG);

    // Create a Matter node and add the mandatory Root Node device type on endpoint 0
    node::config_t node_config;

    // node handle can be used to add/modify other endpoints.
    node_t *node = node::create(&node_config, app_attribute_update_cb, shelly_app_identification_cb);
    ABORT_APP_ON_FAILURE(node != nullptr, ESP_LOGE(TAG, "Failed to create Matter node"));

    MEMORY_PROFILER_DUMP_HEAP_STAT("node created");

    // Two On/Off Lights, relay 1 (O1) on endpoint 1 and relay 2 (O2) on
    // endpoint 2. Latching on/off;
    // SW1, SW2 and the button toggle them. The order of these creates fixes
    // the endpoint IDs, which Matter OTA updates must keep.
    on_off_light::config_t light_config;
    light_config.on_off.on_off = DEFAULT_POWER;

    // endpoint handles can be used to add/modify clusters.
    endpoint_t *endpoint = on_off_light::create(node, &light_config, ENDPOINT_FLAG_NONE, nullptr);
    ABORT_APP_ON_FAILURE(endpoint != nullptr, ESP_LOGE(TAG, "Failed to create on_off_light endpoint"));

    relay_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Light 1 created with endpoint_id %d", relay_endpoint_id);

    // Power metering on each relay's endpoint (Electrical Sensor device
    // type), only on a board that has seen AC mains. Adds clusters, no
    // endpoint. Then hook the OnOff commands so protection can tell
    // deliberate user actions from internal writes.
    metering_add_to_endpoint_ch(0, endpoint);
    protection_attach_endpoint_ch(0, relay_endpoint_id);

    endpoint = on_off_light::create(node, &light_config, ENDPOINT_FLAG_NONE, nullptr);
    ABORT_APP_ON_FAILURE(endpoint != nullptr, ESP_LOGE(TAG, "Failed to create the second on_off_light endpoint"));

    relay2_endpoint_id = endpoint::get_id(endpoint);
    shelly_relay_endpoint_register(1, relay2_endpoint_id);
    ESP_LOGI(TAG, "Light 2 created with endpoint_id %d", relay2_endpoint_id);

    metering_add_to_endpoint_ch(1, endpoint);
    protection_attach_endpoint_ch(1, relay2_endpoint_id);

    // Temperature Sensor endpoint exposes the board NTC (or, if it fails,
    // the die sensor) that the thermal monitor already polls. Starts null
    // until the first reading.
    temperature_sensor::config_t temp_sensor_config;
    temp_sensor_config.temperature_measurement.measured_value = nullable<int16_t>();
    temp_sensor_config.temperature_measurement.min_measured_value = -4000;
    temp_sensor_config.temperature_measurement.max_measured_value = 15000;

    endpoint = temperature_sensor::create(node, &temp_sensor_config, ENDPOINT_FLAG_NONE, nullptr);
    ABORT_APP_ON_FAILURE(endpoint != nullptr, ESP_LOGE(TAG, "Failed to create temperature_sensor endpoint"));

    temperature_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Temperature sensor created with endpoint_id %d", temperature_endpoint_id);

    thermal_set_endpoints(relay_endpoint_id, temperature_endpoint_id);
    thermal_set_relay_endpoint(1, relay2_endpoint_id);

    // Boot reason and hardware faults on the root endpoint's
    // GeneralDiagnostics. Attributes only; no endpoint or cluster is added.
    shelly_diagnostics_add_attributes(node);

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
    const uint16_t relay_endpoints[] = {relay_endpoint_id, relay2_endpoint_id};
    shelly_maintenance_carry_relays(relay_endpoints, 2);

    // An overcurrent trip survives a software reset. Store Off before Matter
    // starts, so neither StartUpOnOff nor the boot defaults can re-energize
    // the load; the protection gate refuses them anyway. After the carry
    // above, which it overrides.
    for (int ch = 0; ch < RELAY_CHANNELS; ch++) {
        if (protection_is_latched_ch(ch)) {
            esp_matter_attr_val_t off = esp_matter_bool(false);
            attribute::set_val(attribute::get(relay_endpoints[ch], chip::app::Clusters::OnOff::Id,
                                              chip::app::Clusters::OnOff::Attributes::OnOff::Id),
                               &off, false);
        }
    }

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

    // Energy accuracy, the restored energy totals, and meter reporting.
    metering_matter_started();

    // Starting driver with default values
    app_driver_relay_set_defaults(relay_endpoint_id);
    app_driver_relay_set_defaults(relay2_endpoint_id);

    // Board data for the Wi-Fi mode page: started only now that Matter is
    // up, so it can't hold up the boot. The meter's registers join it.
    shelly_hw_snapshot_start();

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
