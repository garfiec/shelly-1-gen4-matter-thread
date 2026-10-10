# The upload check, the DNS reply and the QR image are pure C and always
# built (the host tests use them too). The Wi-Fi mode itself only with
# CONFIG_SHELLY_MAINTENANCE.
set(srcs "maintenance_image.c" "maintenance_dns.c" "qr_svg.c" "qrcodegen/qrcodegen.c")
set(embed_txt "")
set(embed_bin "")
if(CONFIG_SHELLY_MAINTENANCE)
    list(APPEND srcs "maintenance.cpp")
    # page.html.gz is built from web/ and committed (docs/BUILDING.md): the
    # firmware build needs no Node.
    set(embed_txt "recovery.html")
    set(embed_bin "page.html.gz")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       EMBED_TXTFILES ${embed_txt}
                       EMBED_FILES ${embed_bin}
                       REQUIRES shelly_app_common
                       PRIV_REQUIRES esp_system esp_wifi esp_netif esp_event esp_http_server esp_timer app_update esp_partition lwip json
                                     shelly_relay shelly_status_led shelly_hw_snapshot shelly_metering)
