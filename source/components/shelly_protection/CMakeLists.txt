idf_component_register(SRCS "protection.cpp" "protection_logic.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_matter esp_timer nvs_flash esp_rom
                                     shelly_relay shelly_app_common)
