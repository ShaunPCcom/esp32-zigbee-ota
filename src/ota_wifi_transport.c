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
#define OTA_URL_MAX_SIZE   2048

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
        .disable_auto_redirect = false,
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
/*  Deferred restart (3 s after successful OTA)                         */
/* ------------------------------------------------------------------ */

static void restart_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Wi-Fi OTA restart");
    esp_restart();
}

static void schedule_restart(void)
{
    esp_timer_handle_t t;
    const esp_timer_create_args_t args = {
        .callback = restart_cb,
        .name     = "ota_rst",
    };
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_once(t, 3000000);  /* 3 s in µs */
    }
}

/* ------------------------------------------------------------------ */
/*  Redirect pre-resolution                                             */
/*                                                                      */
/*  GitHub release URLs redirect: github.com → objects.githubusercontent */
/*  The streaming open()+read() API fails with redirects (0 content-   */
/*  length), and perform() buffers all redirect headers causing "Out of */
/*  buffer" even at 16KB.  Solution: pre-resolve the redirect chain    */
/*  with a lightweight HEAD-style pass that captures Location headers,  */
/*  then download directly from the final CDN URL (no redirects).      */
/* ------------------------------------------------------------------ */

typedef struct {
    char location[OTA_URL_MAX_SIZE];
    bool found;
} location_ctx_t;

static esp_err_t location_event_handler(esp_http_client_event_t *evt)
{
    location_ctx_t *ctx = (location_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_HEADER &&
        strcasecmp(evt->header_key, "Location") == 0) {
        strlcpy(ctx->location, evt->header_value, sizeof(ctx->location));
        ctx->found = true;
    }
    return ESP_OK;
}

/**
 * Follow HTTP redirects for @p url and write the final (200-returning)
 * URL into @p out_url.  Uses disable_auto_redirect=true and captures
 * Location headers via event handler.
 */
static esp_err_t resolve_final_url(const char *url, char *out_url, size_t out_size)
{
    char current[OTA_URL_MAX_SIZE];
    strlcpy(current, url, sizeof(current));

    for (int i = 0; i <= 5; i++) {
        location_ctx_t loc_ctx = { .found = false };

        esp_http_client_config_t cfg = {
            .url                   = current,
            .timeout_ms            = 30000,
            .buffer_size           = 4096,
            .buffer_size_tx        = 4096,
            .crt_bundle_attach     = esp_crt_bundle_attach,
            .event_handler         = location_event_handler,
            .user_data             = &loc_ctx,
            .disable_auto_redirect = true,
        };
        esp_http_client_handle_t c = esp_http_client_init(&cfg);
        if (!c) return ESP_ERR_NO_MEM;

        esp_err_t err = esp_http_client_open(c, 0);
        if (err != ESP_OK) {
            esp_http_client_cleanup(c);
            return err;
        }
        esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);

        /* Drain any body so the connection closes cleanly */
        char drain[64];
        while (esp_http_client_read(c, drain, sizeof(drain)) > 0) {}
        esp_http_client_close(c);
        esp_http_client_cleanup(c);

        if (status == 200) {
            strlcpy(out_url, current, out_size);
            return ESP_OK;
        }
        if (status >= 300 && status < 400 && loc_ctx.found) {
            ESP_LOGI(TAG, "Redirect %d: %s", status, loc_ctx.location);
            strlcpy(current, loc_ctx.location, sizeof(current));
            continue;
        }
        ESP_LOGE(TAG, "Unexpected HTTP %d from %s", status, current);
        return ESP_FAIL;
    }
    ESP_LOGE(TAG, "Too many redirects");
    return ESP_FAIL;
}

/* ------------------------------------------------------------------ */
/*  Download loop (no redirects — always called with final CDN URL)    */
/* ------------------------------------------------------------------ */

static uint8_t s_dl_buf[4096];

static void wifi_ota_task(void *arg)
{
    char *index_url = (char *)arg;  /* strdup'd — must free */

    /* --- 1. Resolve direct .ota URL from index --- */
    char ota_url[OTA_URL_MAX_SIZE];
    esp_err_t err = ota_index_resolve(index_url, ota_url, sizeof(ota_url));
    free(index_url);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Index resolve failed: %s", esp_err_to_name(err));
        ota_state_notify(ZIGBEE_OTA_STATUS_FAILED, 0);
        ota_state_release();
        vTaskDelete(NULL);
        return;
    }

    /* --- 1b. Pre-resolve redirects to get final CDN URL --- */
    char final_url[OTA_URL_MAX_SIZE];
    err = resolve_final_url(ota_url, final_url, sizeof(final_url));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Redirect resolve failed: %s", esp_err_to_name(err));
        ota_state_notify(ZIGBEE_OTA_STATUS_FAILED, 0);
        ota_state_release();
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Final URL: %s", final_url);

    /* --- 2. Prepare partition write --- */
    ota_header_reset(OTA_HEADER_MODE_FULL_FILE);  /* strip 62 bytes: 56-byte file header + 6-byte element header */
    err = ota_writer_begin();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_writer_begin: %s", esp_err_to_name(err));
        ota_state_notify(ZIGBEE_OTA_STATUS_FAILED, 0);
        ota_state_release();
        vTaskDelete(NULL);
        return;
    }
    ota_state_notify(ZIGBEE_OTA_STATUS_START, 0);

    /* --- 3. HTTP GET the final URL (no redirects) --- */
    esp_http_client_config_t http_cfg = {
        .url                   = final_url,
        .timeout_ms            = 60000,
        .buffer_size           = 4096,
        .buffer_size_tx        = 4096,
        .crt_bundle_attach     = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        ota_writer_abort();
        ota_state_notify(ZIGBEE_OTA_STATUS_FAILED, 0);
        ota_state_release();
        vTaskDelete(NULL);
        return;
    }

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        ota_writer_abort();
        ota_state_notify(ZIGBEE_OTA_STATUS_FAILED, 0);
        ota_state_release();
        vTaskDelete(NULL);
        return;
    }

    int content_len = (int)esp_http_client_fetch_headers(client);
    ESP_LOGI(TAG, "OTA file size: %d bytes", content_len);

    /* --- 4. Download + write loop --- */
    int total_read = 0;
    bool ok = true;

    while (true) {
        int r = esp_http_client_read(client, (char *)s_dl_buf, sizeof(s_dl_buf));
        if (r < 0) {
            ESP_LOGE(TAG, "HTTP read error %d", r);
            ok = false;
            break;
        }
        if (r == 0) break;  /* EOF */

        const uint8_t *fw_data;
        size_t fw_len;
        ota_header_process(s_dl_buf, (size_t)r, &fw_data, &fw_len);

        if (fw_len > 0) {
            err = ota_writer_write(fw_data, fw_len);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "ota_writer_write: %s", esp_err_to_name(err));
                ok = false;
                break;
            }
        }

        total_read += r;
        if (content_len > 0) {
            uint8_t pct = (uint8_t)((int64_t)total_read * 100 / content_len);
            ota_state_notify(ZIGBEE_OTA_STATUS_DOWNLOADING, pct);
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    /* --- 5. Finalise or abort --- */
    if (!ok) {
        ota_writer_abort();
        ota_state_notify(ZIGBEE_OTA_STATUS_FAILED, 0);
        ota_state_release();
        vTaskDelete(NULL);
        return;
    }

    err = ota_writer_finish();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_writer_finish: %s", esp_err_to_name(err));
        ota_state_notify(ZIGBEE_OTA_STATUS_FAILED, 0);
        ota_state_release();
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Wi-Fi OTA complete — restarting in 3 s");
    ota_state_notify(ZIGBEE_OTA_STATUS_SUCCESS, 100);
    ota_state_release();
    schedule_restart();
    vTaskDelete(NULL);
}

esp_err_t ota_wifi_transport_start(const char *index_url)
{
    const char *src = (index_url != NULL) ? index_url : OTA_WIFI_DEFAULT_INDEX_URL;

    char *url_copy = strdup(src);
    if (!url_copy) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Spawning Wi-Fi OTA task, index: %s", url_copy);
    BaseType_t created = xTaskCreate(wifi_ota_task, "wifi_ota",
                                     12288, url_copy, 5, NULL);
    if (created != pdPASS) {
        free(url_copy);
        ESP_LOGE(TAG, "Task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
