# The register table is pure C and always built (the host tests check it).
# The probe itself only with CONFIG_SHELLY_ADE7953_PROBE.
set(srcs "ade7953_regs.c")
if(CONFIG_SHELLY_ADE7953_PROBE)
    list(APPEND srcs "ade7953_probe.cpp")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_i2c shelly_hw_snapshot)
