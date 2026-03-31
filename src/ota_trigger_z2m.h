/*
 * SPDX-FileCopyrightText: 2026 Shaun Foulkes
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>

/**
 * @brief Called when Z2M reports a new OTA image is available
 *
 * Stores server address/endpoint in ota_state, then selects the transport:
 *
 *  - ESP32-C6 with Wi-Fi connected and @p wifi_url provided → Wi-Fi transport
 *    (ota_wifi_transport_run spawned in a background task)
 *  - All other cases → Zigbee transport
 *    (returns ESP_OK; the Zigbee SDK then auto-starts Image Block Requests)
 *
 * Acquires the OTA in-progress guard atomically. Returns immediately with
 * ESP_ERR_INVALID_STATE if another OTA is already underway.
 *
 * Called from ota_zigbee_handle_query_image_resp().
 *
 * @param server_addr      Short address of the OTA server (Z2M coordinator)
 * @param server_endpoint  Endpoint of the OTA server
 * @param wifi_url         URL of the .ota image file, or NULL if not provided
 */
esp_err_t ota_trigger_z2m_on_image_available(uint16_t    server_addr,
                                              uint8_t     server_endpoint,
                                              const char *wifi_url);
