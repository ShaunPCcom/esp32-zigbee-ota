/*
 * SPDX-FileCopyrightText: 2026 Shaun Foulkes
 * SPDX-License-Identifier: MIT
 */

/* Web UI OTA trigger — ESP32-C6 only (compiled conditionally via CMakeLists.txt) */

#include "ota_trigger_web.h"
#include "ota_state.h"
#include "ota_wifi_transport.h"
#include "esp_log.h"

static const char *TAG = "ota_trigger_web";

esp_err_t ota_trigger_web_start(const char *url)
{
    /* Atomically claim the OTA slot — rejects concurrent triggers */
    esp_err_t ret = ota_state_acquire(OTA_SOURCE_WIFI);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "OTA already in progress — rejecting web trigger (HTTP 409)");
        return ret;  /* ESP_ERR_INVALID_STATE → caller returns HTTP 409 */
    }

    ESP_LOGI(TAG, "Web UI OTA trigger accepted: %s", url);
    ret = ota_wifi_transport_start(url);
    if (ret != ESP_OK) {
        /* Task creation failed — release the slot so future attempts work */
        ota_state_release();
        return ret;
    }

    return ESP_OK;  /* HTTP 202 — download running in background */
}
