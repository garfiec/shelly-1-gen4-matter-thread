idf_component_register(SRCS "app_common.cpp" "diagnostics.cpp" "shelly_ui.cpp" "ui_policy.c" "shelly_hw_desc.c"
                            "setup_code.c" "shelly_setup_code.cpp" "shelly_settings.cpp"
                            "diag_ring.c" "diag_log.cpp" "calibration_logic.c" "shelly_calibration.cpp"
                            "wifi_password.c" "shelly_wifi_password.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "${ESP_MATTER_PATH}/examples/common/utils"
                       REQUIRES esp_matter
                       PRIV_REQUIRES nvs_flash esp_partition json app_update esp_timer esp_netif openthread shelly_status_led shelly_button shelly_relay)

# The hardware descriptor is only ever read from the image, never
# referenced by code: keep it in the link.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u shelly_hw_desc")
