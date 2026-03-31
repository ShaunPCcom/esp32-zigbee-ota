/*
 * SPDX-FileCopyrightText: 2026 Shaun Foulkes
 * SPDX-License-Identifier: MIT
 */

#pragma once

/* Wi-Fi OTA transport — ESP32-C6 only */

#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief Default OTA index URL
 *
 * GitHub Pages aggregated index. Contains one entry per device, keyed by
 * (manufacturerCode, imageType). The Wi-Fi transport fetches this, finds the
 * entry matching the running device's imageType, and downloads that .ota file.
 */
#define OTA_WIFI_DEFAULT_INDEX_URL \
    "https://shaunpccom.github.io/zigbee-ota-index/ota_index.json"

#ifdef CONFIG_IDF_TARGET_ESP32C6

/**
 * @brief Check whether a Wi-Fi station connection is active
 *
 * Wraps esp_wifi_sta_get_ap_info() so callers (e.g. ota_trigger_z2m) do not
 * need to include esp_wifi.h directly.
 *
 * @return true if Wi-Fi STA is associated with an AP, false otherwise
 */
bool ota_wifi_transport_is_connected(void);

/**
 * @brief Start a Wi-Fi OTA download in a background task
 *
 * Fetches the OTA index from @p index_url, resolves the device's .ota file URL
 * by matching imageType, then downloads and installs the firmware.
 *
 * The OTA in-progress slot must already be acquired by the caller before calling
 * this. The URL string is copied internally — no lifetime constraint on the caller.
 *
 * @param index_url  OTA index JSON URL, or NULL to use OTA_WIFI_DEFAULT_INDEX_URL.
 *
 * @return ESP_OK         Task spawned successfully (download in background)
 * @return ESP_ERR_NO_MEM strdup or task creation failed
 */
esp_err_t ota_wifi_transport_start(const char *index_url);

#endif /* CONFIG_IDF_TARGET_ESP32C6 */
