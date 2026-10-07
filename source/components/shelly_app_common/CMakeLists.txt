idf_component_register(SRCS "app_common.cpp" "diagnostics.cpp" "shelly_ui.cpp" "ui_policy.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "${ESP_MATTER_PATH}/examples/common/utils"
                       REQUIRES esp_matter
                       PRIV_REQUIRES nvs_flash app_update esp_timer shelly_status_led shelly_button)
