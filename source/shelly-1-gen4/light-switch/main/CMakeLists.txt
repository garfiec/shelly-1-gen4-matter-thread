# The Shelly 1 Gen4 app, shared by every product line (../../app). This
# project only sets the product line: its Product ID and default type.
idf_component_register(SRCS              "../../app/app_main.cpp" "../../app/app_driver.cpp"
                       PRIV_INCLUDE_DIRS "../../app" "${ESP_MATTER_PATH}/examples/common/utils")

if (CONFIG_ENABLE_SET_CERT_DECLARATION_API)
    target_add_binary_data(${COMPONENT_TARGET} "certification_declaration/certification_declaration.der" BINARY)
endif()

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
