/*
 * SPDX-FileCopyrightText: 2026 Shaun Foulkes
 * SPDX-License-Identifier: MIT
 */

/* HTTP upload OTA transport — ESP32-C6 only */

#include "ota_upload_transport.h"
#include "esp_http_server.h"
#include "ota_state.h"
#include "ota_writer.h"
#include "ota_header.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <string.h>

static const char *TAG = "ota_upload";

#define UPLOAD_BUF_SIZE 4096

/* ------------------------------------------------------------------ */
/*  Deferred restart (matches wifi transport behaviour)                 */
/* ------------------------------------------------------------------ */

static void restart_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Upload OTA restart");
    esp_restart();
}

static void schedule_restart(void)
{
    esp_timer_handle_t t;
    const esp_timer_create_args_t args = {
        .callback = restart_cb,
        .name     = "ota_up_rst",
    };
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_once(t, 3000000);  /* 3 s in µs */
    }
}

/* ------------------------------------------------------------------ */
/*  Flash from HTTP request body                                        */
/* ------------------------------------------------------------------ */

static uint8_t s_up_buf[UPLOAD_BUF_SIZE];

esp_err_t ota_upload_transport_flash(httpd_req_t *req)
{
    esp_err_t ret = ota_state_acquire(OTA_SOURCE_WIFI);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "OTA already in progress — rejecting upload");
        return ret;
    }

    int content_len = (int)req->content_len;
    ESP_LOGI(TAG, "Upload OTA: %d bytes", content_len);

    ota_header_reset(OTA_HEADER_MODE_FULL_FILE);  /* strip 62-byte .ota file header */
    ret = ota_writer_begin();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ota_writer_begin: %s", esp_err_to_name(ret));
        ota_state_release();
        return ret;
    }

    ota_state_notify(ZIGBEE_OTA_STATUS_START, 0);

    int received = 0;
    bool ok = true;

    while (received < content_len) {
        int to_read = (content_len - received < UPLOAD_BUF_SIZE)
                      ? (content_len - received)
                      : UPLOAD_BUF_SIZE;

        int r = httpd_req_recv(req, (char *)s_up_buf, (size_t)to_read);
        if (r <= 0) {
            ESP_LOGE(TAG, "httpd_req_recv error %d", r);
            ok = false;
            break;
        }

        const uint8_t *fw_data;
        size_t fw_len;
        ota_header_process(s_up_buf, (size_t)r, &fw_data, &fw_len);

        if (fw_len > 0) {
            ret = ota_writer_write(fw_data, fw_len);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "ota_writer_write: %s", esp_err_to_name(ret));
                ok = false;
                break;
            }
        }

        received += r;
        if (content_len > 0) {
            uint8_t pct = (uint8_t)((int64_t)received * 100 / content_len);
            ota_state_notify(ZIGBEE_OTA_STATUS_DOWNLOADING, pct);
        }
    }

    if (!ok) {
        ota_writer_abort();
        ota_state_notify(ZIGBEE_OTA_STATUS_FAILED, 0);
        ota_state_release();
        return ESP_FAIL;
    }

    ret = ota_writer_finish();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ota_writer_finish: %s", esp_err_to_name(ret));
        ota_state_notify(ZIGBEE_OTA_STATUS_FAILED, 0);
        ota_state_release();
        return ret;
    }

    ESP_LOGI(TAG, "Upload OTA complete — restarting in 3 s");
    ota_state_notify(ZIGBEE_OTA_STATUS_SUCCESS, 100);
    ota_state_release();
    schedule_restart();
    return ESP_OK;
}
