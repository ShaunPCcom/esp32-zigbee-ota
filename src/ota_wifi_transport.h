/*
 * SPDX-FileCopyrightText: 2026 Shaun Foulkes
 * SPDX-License-Identifier: MIT
 */

#pragma once

/* Wi-Fi OTA transport — ESP32-C6 only */
#ifdef CONFIG_IDF_TARGET_ESP32C6

#include <stdbool.h>
#include "esp_err.h"

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
 * Spawns a FreeRTOS task that calls ota_wifi_transport_run(url). Returns
 * immediately (HTTP download happens asynchronously).
 *
 * The OTA in-progress slot must already be acquired by the caller
 * (ota_trigger_z2m or ota_trigger_web) before calling this.
 *
 * On completion:  ota_writer_finish() → ota_zigbee_send_upgrade_end(SUCCESS)
 *                 → ota_state_notify(SUCCESS) → ota_state_release()
 *                 → esp_timer one-shot 3 s → esp_restart()
 *
 * On failure:     ota_writer_abort() → ota_zigbee_send_upgrade_end(ABORT)
 *                 → ota_state_notify(FAILED) → ota_state_release()
 *
 * @param url  HTTPS URL of the .ota image file (same file as Zigbee OTA index)
 */
esp_err_t ota_wifi_transport_start(const char *url);

#endif /* CONFIG_IDF_TARGET_ESP32C6 */
