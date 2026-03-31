/*
 * SPDX-FileCopyrightText: 2026 Shaun Foulkes
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Begin OTA partition write
 *
 * Selects the next OTA partition and opens an esp_ota_handle.
 * Must be called before ota_writer_write().
 */
esp_err_t ota_writer_begin(void);

/**
 * @brief Write data to the OTA partition
 *
 * @param data  Pointer to firmware bytes (already header-stripped)
 * @param len   Number of bytes to write
 */
esp_err_t ota_writer_write(const uint8_t *data, size_t len);

/**
 * @brief Finalise and activate the OTA partition
 *
 * Calls esp_ota_end() then esp_ota_set_boot_partition().
 * Does NOT trigger restart — the transport module is responsible for that.
 */
esp_err_t ota_writer_finish(void);

/**
 * @brief Abort OTA write and release partition handle
 *
 * Safe to call when no write is in progress (no-op in that case).
 */
void ota_writer_abort(void);
