idf_component_register(SRCS "button.cpp" "button_logic.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES driver button esp_timer shelly_status_led)
