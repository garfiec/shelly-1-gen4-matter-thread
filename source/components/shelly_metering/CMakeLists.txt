idf_component_register(SRCS "metering.cpp" "metering_logic.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_matter
                       PRIV_REQUIRES esp_timer nvs_flash
                                     shelly_bl0942 shelly_protection shelly_relay)
