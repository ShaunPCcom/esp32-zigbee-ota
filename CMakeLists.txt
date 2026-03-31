set(srcs
    "src/zigbee_ota.c"
    "src/ota_state.c"
    "src/ota_writer.c"
    "src/ota_header.c"
    "src/ota_zigbee_transport.c"
    "src/ota_trigger_z2m.c"
)

set(requires esp-zigbee-lib app_update nvs_flash esp_timer freertos)

if(IDF_TARGET STREQUAL "esp32c6")
    list(APPEND srcs
        "src/ota_wifi_transport.c"
        "src/ota_trigger_web.c"
    )
    list(APPEND requires esp_http_client esp_wifi json)
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES ${requires}
)
