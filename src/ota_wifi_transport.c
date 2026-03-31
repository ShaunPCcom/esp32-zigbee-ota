/*
 * SPDX-FileCopyrightText: 2026 Shaun Foulkes
 * SPDX-License-Identifier: MIT
 */

/* Wi-Fi OTA transport — ESP32-C6 only (compiled conditionally via CMakeLists.txt) */

#include "ota_wifi_transport.h"
#include "ota_state.h"
#include "ota_writer.h"
#include "ota_header.h"
#include "ota_zigbee_transport.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "zigbee_ota.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "ota_wifi";

/* ------------------------------------------------------------------ */
/*  Wi-Fi connection check                                              */
/* ------------------------------------------------------------------ */

bool ota_wifi_transport_is_connected(void)
{
    wifi_ap_record_t ap_info;
    return esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  OTA index fetch + resolve                                           */
/* ------------------------------------------------------------------ */

#define OTA_INDEX_MAX_SIZE 8192
#define OTA_URL_MAX_SIZE   512

/**
 * Fetch @p index_url (HTTPS JSON array), find the entry whose imageType
 * matches the running firmware's image_type from ota_state_get_config(),
 * and write its "url" field into @p out_url.
 */
static esp_err_t ota_index_resolve(const char *index_url,
                                   char *out_url, size_t out_size)
{
    const zigbee_ota_config_t *cfg = ota_state_get_config();
    uint16_t target_image_type = cfg->image_type;

    esp_http_client_config_t http_cfg = {
        .url               = index_url,
        .timeout_ms        = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .follow_redirects  = true,
        .max_redirection_count = 3,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Index fetch open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_fetch_headers(client);

    char *buf = malloc(OTA_INDEX_MAX_SIZE + 1);
    if (!buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int total = 0, r;
    while (total < OTA_INDEX_MAX_SIZE) {
        r = esp_http_client_read(client, buf + total, OTA_INDEX_MAX_SIZE - total);
        if (r <= 0) break;
        total += r;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    buf[total] = '\0';

    if (total == 0) {
        ESP_LOGE(TAG, "Index response empty");
        free(buf);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Index JSON (%d bytes): %s", total, buf);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGE(TAG, "Index JSON parse failed");
        return ESP_FAIL;
    }

    esp_err_t result = ESP_ERR_NOT_FOUND;
    int size = cJSON_GetArraySize(root);
    for (int i = 0; i < size; i++) {
        cJSON *entry  = cJSON_GetArrayItem(root, i);
        cJSON *it     = cJSON_GetObjectItemCaseSensitive(entry, "imageType");
        cJSON *url_j  = cJSON_GetObjectItemCaseSensitive(entry, "url");
        if (cJSON_IsNumber(it) && cJSON_IsString(url_j)) {
            if ((uint16_t)(int)it->valuedouble == target_image_type) {
                strlcpy(out_url, url_j->valuestring, out_size);
                result = ESP_OK;
                break;
            }
        }
    }
    cJSON_Delete(root);

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "No entry for imageType 0x%04x in index", target_image_type);
    } else {
        ESP_LOGI(TAG, "Resolved OTA URL: %s", out_url);
    }
    return result;
}

/* ------------------------------------------------------------------ */
/*  Stubs — download loop implemented in Task 4                        */
/* ------------------------------------------------------------------ */

static void wifi_ota_task(void *arg)
{
    char *index_url = (char *)arg;  /* strdup'd in ota_wifi_transport_start */
    ESP_LOGI(TAG, "Wi-Fi OTA task started — not yet implemented");
    ESP_LOGI(TAG, "  index URL: %s", index_url);
    free(index_url);
    ota_state_notify(ZIGBEE_OTA_STATUS_FAILED, 0);
    ota_state_release();
    vTaskDelete(NULL);
}

esp_err_t ota_wifi_transport_start(const char *index_url)
{
    const char *src = (index_url != NULL) ? index_url : OTA_WIFI_DEFAULT_INDEX_URL;
    char *url_copy = strdup(src);
    if (!url_copy) {
        return ESP_ERR_NO_MEM;
    }
    BaseType_t created = xTaskCreate(wifi_ota_task, "wifi_ota",
                                     8192, url_copy, 5, NULL);
    if (created != pdPASS) {
        free(url_copy);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
