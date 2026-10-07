idf_component_register(SRCS "app_common.cpp" "diagnostics.cpp" "shelly_ui.cpp" "ui_policy.c" "shelly_hw_desc.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "${ESP_MATTER_PATH}/examples/common/utils"
                       REQUIRES esp_matter
                       PRIV_REQUIRES nvs_flash app_update esp_timer shelly_status_led shelly_button shelly_relay)

# The hardware descriptor is only ever read from the image, never
# referenced by code: keep it in the link.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u shelly_hw_desc")
