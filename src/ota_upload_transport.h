/*
 * SPDX-FileCopyrightText: 2026 Shaun Foulkes
 * SPDX-License-Identifier: MIT
 */

#pragma once

/* HTTP upload OTA transport — ESP32-C6 only */
#ifdef CONFIG_IDF_TARGET_ESP32C6

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * @brief Flash firmware from an HTTP POST upload request
 *
 * Reads the .ota file body directly from @p req, strips the 62-byte header,
 * writes firmware to the next OTA partition, and schedules a restart on success.
 * Runs synchronously in the HTTP server task — the connection stays open until
 * flashing is complete (or fails).
 *
 * @param req  Active httpd request with Content-Length set and body unread
 *
 * @return
 *   ESP_OK               Flash complete — restart scheduled
 *   ESP_ERR_INVALID_STATE OTA already in progress
 *   ESP_FAIL             Flash or receive error
 */
esp_err_t ota_upload_transport_flash(httpd_req_t *req);

#endif /* CONFIG_IDF_TARGET_ESP32C6 */
