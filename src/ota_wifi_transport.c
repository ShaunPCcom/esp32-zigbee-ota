/*
 * SPDX-FileCopyrightText: 2026 Shaun Foulkes
 * SPDX-License-Identifier: MIT
 */

/* Wi-Fi OTA transport — ESP32-C6 only (compiled conditionally via CMakeLists.txt) */

#include "ota_wifi_transport.h"
#include "ota_state.h"
#include "ota_writer.h"
#include "ota_header.h"
#include "ota_zigbee_transport.h"
#include "esp_log.h"

static const char *TAG = "ota_wifi_transport";

bool ota_wifi_transport_is_connected(void)
{
    /* TODO: implement using esp_wifi_sta_get_ap_info() once Wi-Fi OTA transport
     * is implemented. Currently always returns false because wifi_url is always
     * NULL (URL extraction from Z2M query response not yet implemented). */
    return false;
}

/* TODO: Implement Wi-Fi OTA transport
 *
 * This module will:
 *   1. Download the .ota file from the URL provided via esp_http_client
 *   2. Use ota_header_reset(OTA_HEADER_MODE_FULL_FILE) to skip the 62-byte
 *      header (56-byte Zigbee OTA file header + 6-byte element header)
 *   3. Write each chunk via ota_writer_write() after ota_header_process()
 *   4. On success: ota_writer_finish() → ota_zigbee_send_upgrade_end(SUCCESS)
 *                  → ota_state_notify(SUCCESS) → ota_state_release()
 *                  → esp_timer one-shot 3 s → esp_restart()
 *   5. On failure: ota_writer_abort() → ota_zigbee_send_upgrade_end(ABORT)
 *                  → ota_state_notify(FAILED) → ota_state_release()
 *
 * Note: restart uses esp_timer (not esp_zb_scheduler_alarm) since we're in a
 * FreeRTOS task context, not the Zigbee stack task.
 *
 * Note: ota_zigbee_send_upgrade_end() informs Z2M the OTA is done, causing it
 * to re-interview the device and read the updated sw_build_id. No synthetic
 * Image Block Requests are sent — Wi-Fi OTA completes in minutes so 0% → done
 * is acceptable.
 */

static void wifi_ota_task(void *arg)
{
    const char *url = (const char *)arg;
    ESP_LOGI(TAG, "Wi-Fi OTA task started (not yet implemented)");
    ESP_LOGI(TAG, "  URL: %s", url);

    /* TODO: implement HTTP download loop */

    ESP_LOGE(TAG, "Wi-Fi OTA transport not implemented — releasing slot");
    ota_state_notify(ZIGBEE_OTA_STATUS_FAILED, 0);
    ota_state_release();
    vTaskDelete(NULL);
}

esp_err_t ota_wifi_transport_start(const char *url)
{
    ESP_LOGI(TAG, "Spawning Wi-Fi OTA task for: %s", url);

    /* url will remain valid for the lifetime of the task since it points
     * to the Z2M query response string which is stable until next restart */
    BaseType_t created = xTaskCreate(wifi_ota_task, "wifi_ota",
                                     8192, (void *)url, 5, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi OTA task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
