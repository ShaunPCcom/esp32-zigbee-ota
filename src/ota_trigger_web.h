/*
 * SPDX-FileCopyrightText: 2026 Shaun Foulkes
 * SPDX-License-Identifier: MIT
 */

#pragma once

/* Web UI OTA trigger — ESP32-C6 only */
#ifdef CONFIG_IDF_TARGET_ESP32C6

#include "esp_err.h"

/**
 * @brief Trigger a Wi-Fi OTA download from the web server handler
 *
 * Called by the HTTP server when the web UI submits an OTA request.
 * Acquires the OTA slot atomically and spawns a background download task.
 * Returns immediately so the HTTP response can be sent without blocking.
 *
 * @param url  HTTPS URL of the .ota image file
 *
 * @return
 *   ESP_OK               Accepted (HTTP 202) — download started in background
 *   ESP_ERR_INVALID_STATE Already in progress (HTTP 409) — reject request
 *   ESP_ERR_NO_MEM       Task creation failed
 */
esp_err_t ota_trigger_web_start(const char *url);

#endif /* CONFIG_IDF_TARGET_ESP32C6 */
