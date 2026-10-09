# The upload check, the DNS reply and the QR image are pure C and always
# built (the host tests use them too). The Wi-Fi mode itself only with
# CONFIG_SHELLY_MAINTENANCE.
set(srcs "maintenance_image.c" "maintenance_dns.c" "qr_svg.c" "qrcodegen/qrcodegen.c")
set(embed "")
if(CONFIG_SHELLY_MAINTENANCE)
    list(APPEND srcs "maintenance.cpp")
    set(embed "page.html")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       EMBED_TXTFILES ${embed}
                       REQUIRES shelly_app_common
                       PRIV_REQUIRES esp_system esp_wifi esp_netif esp_event esp_http_server esp_timer app_update esp_partition lwip
                                     shelly_relay shelly_status_led shelly_hw_snapshot shelly_metering)
