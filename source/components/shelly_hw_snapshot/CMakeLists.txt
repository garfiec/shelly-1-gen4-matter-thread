# The snapshot's data and text forms are pure C and always built (the host
# tests use them too). Sampling only with CONFIG_SHELLY_HW_SNAPSHOT.
set(srcs "hw_snapshot.c")
if(CONFIG_SHELLY_HW_SNAPSHOT)
    list(APPEND srcs "shelly_hw_snapshot.cpp")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES driver esp_system esp_timer heap
                                     shelly_relay shelly_switch_input shelly_thermal)
