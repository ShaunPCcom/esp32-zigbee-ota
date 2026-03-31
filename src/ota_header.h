/*
 * SPDX-FileCopyrightText: 2026 Shaun Foulkes
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * @brief OTA image header strip mode
 *
 * The Zigbee OTA .ota file layout:
 *   Bytes  0–55:  56-byte Zigbee OTA file header
 *   Bytes 56–57:  2-byte element Tag ID
 *   Bytes 58–61:  4-byte element Length
 *   Bytes 62+:    ESP32 firmware binary (starts with 0xE9 magic)
 *
 * When data arrives via Zigbee block transfer the stack pre-strips the 56-byte
 * file header before the component sees the data — only the 6-byte element
 * header + firmware payload is present.  Strip = 6 bytes.
 *
 * When the full .ota file is downloaded over Wi-Fi, all bytes are present.
 * Strip = 56 + 6 = 62 bytes.
 */
typedef enum {
    OTA_HEADER_MODE_ZIGBEE,     /*!< Zigbee stack pre-stripped 56B; skip first 6 bytes */
    OTA_HEADER_MODE_FULL_FILE,  /*!< Full .ota file; skip first 62 bytes */
} ota_header_mode_t;

/**
 * @brief Reset header state for a new OTA transfer
 *
 * Must be called once at the start of each OTA operation before the first
 * ota_header_process() call.
 *
 * @param mode  Strip mode (6 or 62 bytes)
 */
void ota_header_reset(ota_header_mode_t mode);

/**
 * @brief Strip header bytes from an incoming chunk and return firmware payload
 *
 * On the first chunk (or first few chunks if they are very small), strips the
 * required header bytes.  All subsequent chunks pass through unchanged.
 *
 * Thread-safe for a single OTA in progress (no concurrent calls assumed).
 *
 * @param[in]  in        Pointer to raw incoming bytes
 * @param[in]  in_len    Number of bytes in the input buffer
 * @param[out] out_data  Set to the start of firmware payload within @p in
 *                       (NULL if the entire chunk was header bytes)
 * @param[out] out_len   Number of firmware payload bytes (0 if all were header)
 */
void ota_header_process(const uint8_t *in, size_t in_len,
                        const uint8_t **out_data, size_t *out_len);
