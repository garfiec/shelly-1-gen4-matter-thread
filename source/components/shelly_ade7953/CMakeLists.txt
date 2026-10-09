# The register table and the codec are pure C and always built (the host
# tests check them). The driver only with CONFIG_SHELLY_HAS_ADE7953.
set(srcs "ade7953_regs.c" "ade7953_codec.c")
if(CONFIG_SHELLY_HAS_ADE7953)
    list(APPEND srcs "ade7953.cpp")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_i2c esp_timer shelly_hw_snapshot)
