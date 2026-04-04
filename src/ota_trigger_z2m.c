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

    /* Zigbee transport: server info already stored above.
     * Do NOT acquire the OTA slot here — the slot is acquired in the
     * OTA_START callback once the Zigbee SDK actually begins block transfer.
     * Acquiring here leaves the slot permanently held if START never fires
     * (e.g. a Z2M "check for update" produces a query response but does not
     * initiate block transfer, unlike a Z2M "trigger update"). */
    ESP_LOGI(TAG, "Zigbee OTA image available on server 0x%04x ep %u — awaiting START",
             server_addr, server_endpoint);
    return ESP_OK;
}
