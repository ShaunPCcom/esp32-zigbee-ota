idf_component_register(
    SRCS "src/zigbee_ota.c"
    INCLUDE_DIRS "include"
    REQUIRES esp-zigbee-lib app_update nvs_flash esp_timer
)
