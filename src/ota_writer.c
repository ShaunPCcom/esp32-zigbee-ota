/*
 * SPDX-FileCopyrightText: 2026 Shaun Foulkes
 * SPDX-License-Identifier: MIT
 */

#include "ota_writer.h"
#include "esp_log.h"
#include "esp_ota_ops.h"

static const char *TAG = "ota_writer";

static const esp_partition_t *s_partition = NULL;
static esp_ota_handle_t       s_handle    = 0;

esp_err_t ota_writer_begin(void)
{
    s_partition = esp_ota_get_next_update_partition(NULL);
    if (!s_partition) {
        ESP_LOGE(TAG, "No OTA partition available");
        return ESP_FAIL;
    }

    esp_err_t ret = esp_ota_begin(s_partition, OTA_WITH_SEQUENTIAL_WRITES, &s_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(ret));
        s_partition = NULL;
        s_handle    = 0;
        return ret;
    }

    ESP_LOGI(TAG, "OTA write begun: partition '%s' at 0x%lx (%lu bytes)",
             s_partition->label, s_partition->address, s_partition->size);
    return ESP_OK;
}

esp_err_t ota_writer_write(const uint8_t *data, size_t len)
{
    if (!s_handle) {
        ESP_LOGE(TAG, "ota_writer_write called without active handle");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_ota_write(s_handle, data, len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t ota_writer_finish(void)
{
    if (!s_handle) {
        ESP_LOGE(TAG, "ota_writer_finish called without active handle");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_ota_end(s_handle);
    s_handle = 0;
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(ret));
        s_partition = NULL;
        return ret;
    }

    ret = esp_ota_set_boot_partition(s_partition);
    s_partition = NULL;
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "OTA partition finalised and set as next boot partition");
    return ESP_OK;
}

void ota_writer_abort(void)
{
    if (s_handle) {
        esp_ota_abort(s_handle);
        s_handle    = 0;
        s_partition = NULL;
        ESP_LOGW(TAG, "OTA write aborted");
    }
}
