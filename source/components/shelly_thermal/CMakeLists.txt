idf_component_register(SRCS "thermal.cpp" "thermal_logic.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_matter esp_timer esp_driver_tsens esp_adc
                                     shelly_relay shelly_app_common)
