/*
 * SPDX-FileCopyrightText: 2026 Shaun Foulkes
 * SPDX-License-Identifier: MIT
 */

#include "ota_header.h"
#include "esp_log.h"

static const char *TAG = "ota_header";

/* Bytes to skip at the start of the OTA data stream */
#define HEADER_SKIP_ZIGBEE    6    /* 2-byte Tag ID + 4-byte Tag Length (element header) */
#define HEADER_SKIP_FULL_FILE 62   /* 56-byte OTA file header + 6-byte element header */

static size_t s_skip_total   = 0;
static size_t s_bytes_consumed = 0;

void ota_header_reset(ota_header_mode_t mode)
{
    s_skip_total     = (mode == OTA_HEADER_MODE_FULL_FILE) ? HEADER_SKIP_FULL_FILE
                                                           : HEADER_SKIP_ZIGBEE;
    s_bytes_consumed = 0;
    ESP_LOGI(TAG, "Header reset: mode=%s, skip=%zu bytes",
             (mode == OTA_HEADER_MODE_FULL_FILE) ? "FULL_FILE" : "ZIGBEE",
             s_skip_total);
}

void ota_header_process(const uint8_t *in, size_t in_len,
                        const uint8_t **out_data, size_t *out_len)
{
    *out_data = NULL;
    *out_len  = 0;

    if (in_len == 0) {
        return;
    }

    if (s_bytes_consumed >= s_skip_total) {
        /* Header already consumed — pass through unchanged */
        *out_data = in;
        *out_len  = in_len;
        return;
    }

    /* How many header bytes remain to skip? */
    size_t remaining_skip = s_skip_total - s_bytes_consumed;

    if (in_len <= remaining_skip) {
        /* Entire chunk is still header bytes */
        s_bytes_consumed += in_len;
        ESP_LOGD(TAG, "Consumed %zu header bytes (%zu / %zu total)",
                 in_len, s_bytes_consumed, s_skip_total);
        return;
    }

    /* Partial chunk: skip the remaining header bytes, return the rest */
    s_bytes_consumed = s_skip_total;
    *out_data = in + remaining_skip;
    *out_len  = in_len - remaining_skip;
    ESP_LOGI(TAG, "Header stripped (%zu bytes); first firmware byte: 0x%02x",
             s_skip_total, (*out_data)[0]);
}
