/*
 * SPDX-FileCopyrightText: 2026 Shaun Foulkes
 * SPDX-License-Identifier: MIT
 */

#include "zigbee_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_zigbee_cluster.h"
#include "esp_zigbee_ota.h"

static const char *TAG = "zigbee_ota";

/* OTA state */
static struct {
    bool initialized;
    uint8_t endpoint;
    zigbee_ota_config_t config;
    zigbee_ota_status_callback_t status_callback;

    /* OTA operation state */
    const esp_partition_t *ota_partition;
    esp_ota_handle_t ota_handle;
    uint32_t total_size;
    uint32_t downloaded_size;
    int64_t start_time_us;
} s_ota_state = {
    .initialized = false,
    .endpoint = 0,
    .status_callback = NULL,
    .ota_partition = NULL,
    .ota_handle = 0,
    .total_size = 0,
    .downloaded_size = 0,
    .start_time_us = 0,
};

/* Forward declarations */
static esp_err_t ota_upgrade_status_handler(esp_zb_zcl_ota_upgrade_value_message_t message);
static esp_err_t ota_upgrade_query_image_resp_handler(esp_zb_zcl_ota_upgrade_query_image_resp_message_t message);

/**
 * @brief Invoke status callback if registered
 */
static void invoke_status_callback(zigbee_ota_status_t status, uint8_t progress_percent)
{
    if (s_ota_state.status_callback) {
        s_ota_state.status_callback(status, progress_percent);
    }
}

/**
 * @brief Calculate download progress percentage
 */
static uint8_t calculate_progress(uint32_t downloaded, uint32_t total)
{
    if (total == 0) {
        return 0;
    }
    uint32_t progress = (downloaded * 100) / total;
    return (progress > 100) ? 100 : (uint8_t)progress;
}

/**
 * @brief Handle OTA upgrade status updates
 */
static esp_err_t ota_upgrade_status_handler(esp_zb_zcl_ota_upgrade_value_message_t message)
{
    esp_err_t ret = ESP_OK;

    if (message.info.status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "OTA status callback with ZCL error: 0x%x", message.info.status);
        return ESP_FAIL;
    }

    switch (message.upgrade_status) {
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_START:
        ESP_LOGI(TAG, "OTA upgrade started");
        s_ota_state.start_time_us = esp_timer_get_time();
        s_ota_state.total_size = 0;
        s_ota_state.downloaded_size = 0;

        /* Get next OTA partition */
        s_ota_state.ota_partition = esp_ota_get_next_update_partition(NULL);
        if (!s_ota_state.ota_partition) {
            ESP_LOGE(TAG, "No OTA partition found");
            invoke_status_callback(ZIGBEE_OTA_STATUS_FAILED, 0);
            return ESP_FAIL;
        }

        /* Begin OTA operation */
        ret = esp_ota_begin(s_ota_state.ota_partition, OTA_WITH_SEQUENTIAL_WRITES, &s_ota_state.ota_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(ret));
            invoke_status_callback(ZIGBEE_OTA_STATUS_FAILED, 0);
            return ret;
        }

        invoke_status_callback(ZIGBEE_OTA_STATUS_START, 0);
        ESP_LOGI(TAG, "OTA partition: %s, offset: 0x%lx, size: %lu bytes",
                 s_ota_state.ota_partition->label,
                 s_ota_state.ota_partition->address,
                 s_ota_state.ota_partition->size);
        break;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_RECEIVE:
        s_ota_state.total_size = message.ota_header.image_size;
        s_ota_state.downloaded_size += message.payload_size;

        /* Write data to OTA partition */
        if (message.payload_size && message.payload) {
            const uint8_t *write_data = (const uint8_t *)message.payload;
            uint16_t write_size = message.payload_size;

            /* First chunk: skip OTA element tag header (2 bytes tag ID + 4 bytes length) */
            if (s_ota_state.downloaded_size == message.payload_size) {
                #define OTA_ELEMENT_TAG_HEADER_LEN 6
                if (message.payload_size > OTA_ELEMENT_TAG_HEADER_LEN) {
                    write_data += OTA_ELEMENT_TAG_HEADER_LEN;
                    write_size -= OTA_ELEMENT_TAG_HEADER_LEN;
                    ESP_LOGI(TAG, "Stripped %d-byte OTA element header from first chunk", OTA_ELEMENT_TAG_HEADER_LEN);
                }
            }

            ret = esp_ota_write(s_ota_state.ota_handle, write_data, write_size);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(ret));
                invoke_status_callback(ZIGBEE_OTA_STATUS_FAILED, 0);
                return ret;
            }
        }

        uint8_t progress = calculate_progress(s_ota_state.downloaded_size, s_ota_state.total_size);
        ESP_LOGI(TAG, "OTA progress: %u%% (%lu / %lu bytes)",
                 progress, s_ota_state.downloaded_size, s_ota_state.total_size);
        invoke_status_callback(ZIGBEE_OTA_STATUS_DOWNLOADING, progress);
        break;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_APPLY:
        ESP_LOGI(TAG, "OTA upgrade apply");
        invoke_status_callback(ZIGBEE_OTA_STATUS_APPLYING, 100);
        break;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_CHECK:
        /* Validate download completed successfully */
        ret = (s_ota_state.downloaded_size == s_ota_state.total_size) ? ESP_OK : ESP_FAIL;
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "OTA download incomplete: %lu / %lu bytes",
                     s_ota_state.downloaded_size, s_ota_state.total_size);
            invoke_status_callback(ZIGBEE_OTA_STATUS_FAILED, 0);
        } else {
            ESP_LOGI(TAG, "OTA download complete, validation passed");
            invoke_status_callback(ZIGBEE_OTA_STATUS_DOWNLOAD_COMPLETE, 100);
        }

        /* Reset counters */
        s_ota_state.downloaded_size = 0;
        s_ota_state.total_size = 0;
        break;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_FINISH:
        {
            int64_t duration_ms = (esp_timer_get_time() - s_ota_state.start_time_us) / 1000;
            ESP_LOGI(TAG, "OTA upgrade finished");
            ESP_LOGI(TAG, "  Version: 0x%08lx", message.ota_header.file_version);
            ESP_LOGI(TAG, "  Manufacturer: 0x%04x", message.ota_header.manufacturer_code);
            ESP_LOGI(TAG, "  Image type: 0x%04x", message.ota_header.image_type);
            ESP_LOGI(TAG, "  Size: %lu bytes", message.ota_header.image_size);
            ESP_LOGI(TAG, "  Duration: %lld ms", duration_ms);

            /* End OTA operation */
            ret = esp_ota_end(s_ota_state.ota_handle);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(ret));
                invoke_status_callback(ZIGBEE_OTA_STATUS_FAILED, 0);
                return ret;
            }

            /* Set boot partition */
            ret = esp_ota_set_boot_partition(s_ota_state.ota_partition);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(ret));
                invoke_status_callback(ZIGBEE_OTA_STATUS_FAILED, 0);
                return ret;
            }

            invoke_status_callback(ZIGBEE_OTA_STATUS_SUCCESS, 100);
            ESP_LOGW(TAG, "OTA complete, restarting...");
            esp_restart();
        }
        break;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ABORT:
        ESP_LOGW(TAG, "OTA upgrade aborted");
        invoke_status_callback(ZIGBEE_OTA_STATUS_FAILED, 0);
        s_ota_state.downloaded_size = 0;
        s_ota_state.total_size = 0;
        ret = ESP_FAIL;
        break;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_SERVER_NOT_FOUND:
        ESP_LOGW(TAG, "OTA server not found");
        invoke_status_callback(ZIGBEE_OTA_STATUS_SERVER_NOT_FOUND, 0);
        ret = ESP_FAIL;
        break;

    default:
        ESP_LOGI(TAG, "OTA status: %d", message.upgrade_status);
        break;
    }

    return ret;
}

/**
 * @brief Handle OTA query image response
 */
static esp_err_t ota_upgrade_query_image_resp_handler(esp_zb_zcl_ota_upgrade_query_image_resp_message_t message)
{
    if (message.info.status == ESP_ZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "OTA image available:");
        ESP_LOGI(TAG, "  Server: 0x%04x, endpoint: %d",
                 message.server_addr.u.short_addr, message.server_endpoint);
        ESP_LOGI(TAG, "  Version: 0x%08lx (current: 0x%08lx)",
                 message.file_version, s_ota_state.config.current_file_version);
        ESP_LOGI(TAG, "  Manufacturer: 0x%04x", message.manufacturer_code);
        ESP_LOGI(TAG, "  Size: %lu bytes", message.image_size);
        ESP_LOGI(TAG, "Approving OTA image upgrade");
        return ESP_OK;
    } else {
        ESP_LOGI(TAG, "No OTA update available (status: 0x%x)", message.info.status);
        return ESP_OK;  /* Not an error, just no update */
    }
}

/* Public API implementations */

esp_err_t zigbee_ota_init(esp_zb_cluster_list_t *cluster_list, uint8_t endpoint, const zigbee_ota_config_t *config)
{
    if (!cluster_list || !config) {
        ESP_LOGE(TAG, "Invalid arguments");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_ota_state.initialized) {
        ESP_LOGW(TAG, "OTA already initialized");
        return ESP_OK;
    }

    /* Store configuration */
    s_ota_state.config = *config;
    s_ota_state.endpoint = endpoint;

    /* Create OTA cluster configuration */
    esp_zb_ota_cluster_cfg_t ota_cluster_cfg = {
        .ota_upgrade_file_version = config->current_file_version,
        .ota_upgrade_downloaded_file_ver = ESP_ZB_ZCL_OTA_UPGRADE_DOWNLOADED_FILE_VERSION_DEF_VALUE,
        .ota_upgrade_manufacturer = config->manufacturer_code,
        .ota_upgrade_image_type = config->image_type,
    };

    /* Create OTA cluster */
    esp_zb_attribute_list_t *ota_cluster = esp_zb_ota_cluster_create(&ota_cluster_cfg);
    if (!ota_cluster) {
        ESP_LOGE(TAG, "Failed to create OTA cluster");
        return ESP_ERR_NO_MEM;
    }

    /* Add client variable configuration */
    esp_zb_zcl_ota_upgrade_client_variable_t variable_config = {
        .timer_query = config->query_interval_minutes,
        .hw_version = config->hw_version,
        .max_data_size = config->max_data_size,
    };

    uint16_t ota_server_addr = 0xffff;
    uint8_t ota_server_ep = 0xff;

    esp_err_t ret;
    ret = esp_zb_ota_cluster_add_attr(ota_cluster, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_CLIENT_DATA_ID, &variable_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add OTA client data attr: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_zb_ota_cluster_add_attr(ota_cluster, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_SERVER_ADDR_ID, &ota_server_addr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add OTA server addr attr: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_zb_ota_cluster_add_attr(ota_cluster, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_SERVER_ENDPOINT_ID, &ota_server_ep);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add OTA server endpoint attr: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Add OTA cluster to cluster list as client role */
    ret = esp_zb_cluster_list_add_ota_cluster(cluster_list, ota_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add OTA cluster to list: %s", esp_err_to_name(ret));
        return ret;
    }

    s_ota_state.initialized = true;
    ESP_LOGI(TAG, "OTA client initialized on endpoint %d", endpoint);
    ESP_LOGI(TAG, "  Manufacturer: 0x%04x", config->manufacturer_code);
    ESP_LOGI(TAG, "  Image type: 0x%04x", config->image_type);
    ESP_LOGI(TAG, "  Current version: 0x%08lx", config->current_file_version);
    ESP_LOGI(TAG, "  Query interval: %u minutes", config->query_interval_minutes);

    return ESP_OK;
}

esp_err_t zigbee_ota_register_status_callback(zigbee_ota_status_callback_t callback)
{
    if (!s_ota_state.initialized) {
        ESP_LOGE(TAG, "OTA not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    s_ota_state.status_callback = callback;
    ESP_LOGI(TAG, "Status callback %s", callback ? "registered" : "unregistered");
    return ESP_OK;
}

esp_err_t zigbee_ota_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    if (!s_ota_state.initialized) {
        return ESP_ERR_NOT_SUPPORTED;  /* Not initialized, let app handle it */
    }

    esp_err_t ret = ESP_ERR_NOT_SUPPORTED;

    switch (callback_id) {
    case ESP_ZB_CORE_OTA_UPGRADE_VALUE_CB_ID:
        ret = ota_upgrade_status_handler(*(esp_zb_zcl_ota_upgrade_value_message_t *)message);
        break;

    case ESP_ZB_CORE_OTA_UPGRADE_QUERY_IMAGE_RESP_CB_ID:
        ret = ota_upgrade_query_image_resp_handler(*(esp_zb_zcl_ota_upgrade_query_image_resp_message_t *)message);
        break;

    default:
        /* Not an OTA callback, application should handle it */
        ret = ESP_ERR_NOT_SUPPORTED;
        break;
    }

    return ret;
}

esp_err_t zigbee_ota_start_query(uint16_t server_addr, uint8_t server_endpoint)
{
    if (!s_ota_state.initialized) {
        ESP_LOGE(TAG, "OTA not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Querying OTA server 0x%04x endpoint %d", server_addr, server_endpoint);
    esp_err_t ret = esp_zb_ota_upgrade_client_query_image_req(server_addr, server_endpoint);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to query OTA server: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t zigbee_ota_set_query_interval(uint16_t interval_minutes)
{
    if (!s_ota_state.initialized) {
        ESP_LOGE(TAG, "OTA not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_zb_ota_upgrade_client_query_interval_set(s_ota_state.endpoint, interval_minutes);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set query interval: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    s_ota_state.config.query_interval_minutes = interval_minutes;
    ESP_LOGI(TAG, "Query interval set to %u minutes", interval_minutes);
    return ESP_OK;
}
