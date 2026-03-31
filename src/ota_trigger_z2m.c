/*
 * SPDX-FileCopyrightText: 2026 Shaun Foulkes
 * SPDX-License-Identifier: MIT
 */

#include "ota_trigger_z2m.h"
#include "ota_state.h"
#include "esp_log.h"

#if CONFIG_IDF_TARGET_ESP32C6
#include "ota_wifi_transport.h"
#endif

static const char *TAG = "ota_trigger_z2m";

esp_err_t ota_trigger_z2m_on_image_available(uint16_t    server_addr,
                                              uint8_t     server_endpoint,
                                              const char *wifi_url)
{
    /* Store server info before acquiring — used by retry and upgrade-end logic */
    ota_state_set_server(server_addr, server_endpoint);

#if CONFIG_IDF_TARGET_ESP32C6
    /* On C6: prefer Wi-Fi transport when a station connection is active.
     * wifi_url from the Z2M query response is always NULL (field not present in SDK).
     * The Wi-Fi transport resolves the .ota URL from its built-in index. */
    if (ota_wifi_transport_is_connected()) {
        esp_err_t ret = ota_state_acquire(OTA_SOURCE_WIFI);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "OTA already in progress, ignoring Z2M notification");
            return ret;
        }
        ESP_LOGI(TAG, "Wi-Fi connected — using Wi-Fi OTA transport");
        return ota_wifi_transport_start(ota_state_get_wifi_index_url());
    }
    ESP_LOGI(TAG, "Wi-Fi not connected — falling back to Zigbee OTA");
#endif /* CONFIG_IDF_TARGET_ESP32C6 */

    /* Zigbee transport: acquire slot then return ESP_OK.
     * The Zigbee SDK auto-starts Image Block Requests after the query
     * image response is acknowledged — no further action needed here. */
    esp_err_t ret = ota_state_acquire(OTA_SOURCE_ZIGBEE);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "OTA already in progress, ignoring Z2M notification");
        return ret;
    }

    ESP_LOGI(TAG, "Using Zigbee OTA transport (server 0x%04x ep %u)",
             server_addr, server_endpoint);
    return ESP_OK;
}
