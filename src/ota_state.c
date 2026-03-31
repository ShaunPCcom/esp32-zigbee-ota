/*
 * SPDX-FileCopyrightText: 2026 Shaun Foulkes
 * SPDX-License-Identifier: MIT
 */

#include "ota_state.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static portMUX_TYPE s_in_progress_mux = portMUX_INITIALIZER_UNLOCKED;

static const char *TAG = "ota_state";

static struct {
    bool                         initialized;
    uint8_t                      endpoint;
    zigbee_ota_config_t          config;
    zigbee_ota_status_callback_t callback;

    /* In-progress guard — modified only under critical section */
    volatile bool                in_progress;
    ota_source_t                 source;

    /* OTA server address (populated by ota_trigger_z2m) */
    uint16_t                     server_addr;
    uint8_t                      server_endpoint;

    /* Retry counter */
    uint8_t                      abort_retries;

    /* Wi-Fi OTA index URL override (NULL = use built-in default) */
    char                         wifi_index_url[256];
} s_state = {
    .initialized    = false,
    .server_addr    = 0xffff,
    .server_endpoint = 0xff,
};

void ota_state_init(const zigbee_ota_config_t *config, uint8_t endpoint)
{
    s_state.config       = *config;
    s_state.endpoint     = endpoint;
    s_state.initialized  = true;
    s_state.in_progress  = false;
    s_state.source       = OTA_SOURCE_NONE;
    s_state.abort_retries = 0;
    s_state.server_addr  = 0xffff;
    s_state.server_endpoint = 0xff;
    ESP_LOGI(TAG, "OTA state initialised (endpoint %d)", endpoint);
}

void ota_state_set_callback(zigbee_ota_status_callback_t cb)
{
    s_state.callback = cb;
}

bool ota_state_is_in_progress(void)
{
    return s_state.in_progress;
}

esp_err_t ota_state_acquire(ota_source_t source)
{
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    taskENTER_CRITICAL(&s_in_progress_mux);
    if (!s_state.in_progress) {
        s_state.in_progress = true;
        s_state.source      = source;
        ret = ESP_OK;
    }
    taskEXIT_CRITICAL(&s_in_progress_mux);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA slot acquired (source %d)", (int)source);
    } else {
        ESP_LOGW(TAG, "OTA already in progress (source %d), rejecting new request (source %d)",
                 (int)s_state.source, (int)source);
    }
    return ret;
}

void ota_state_release(void)
{
    s_state.in_progress = false;
    s_state.source      = OTA_SOURCE_NONE;
    ESP_LOGI(TAG, "OTA slot released");
}

ota_source_t ota_state_get_source(void)
{
    return s_state.source;
}

void ota_state_notify(zigbee_ota_status_t status, uint8_t progress_pct)
{
    if (s_state.callback) {
        s_state.callback(status, progress_pct);
    }
}

void ota_state_set_server(uint16_t addr, uint8_t endpoint)
{
    s_state.server_addr     = addr;
    s_state.server_endpoint = endpoint;
}

uint16_t ota_state_get_server_addr(void)
{
    return s_state.server_addr;
}

uint8_t ota_state_get_server_endpoint(void)
{
    return s_state.server_endpoint;
}

uint8_t ota_state_get_abort_retries(void)
{
    return s_state.abort_retries;
}

void ota_state_inc_abort_retries(void)
{
    s_state.abort_retries++;
}

void ota_state_reset_abort_retries(void)
{
    s_state.abort_retries = 0;
}

const zigbee_ota_config_t *ota_state_get_config(void)
{
    return &s_state.config;
}

uint8_t ota_state_get_endpoint(void)
{
    return s_state.endpoint;
}

void ota_state_set_wifi_index_url(const char *url)
{
    if (url && url[0]) {
        strlcpy(s_state.wifi_index_url, url, sizeof(s_state.wifi_index_url));
    } else {
        s_state.wifi_index_url[0] = '\0';
    }
}

const char *ota_state_get_wifi_index_url(void)
{
    return s_state.wifi_index_url[0] ? s_state.wifi_index_url : NULL;
}
