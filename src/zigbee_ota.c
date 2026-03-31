/*
 * SPDX-FileCopyrightText: 2026 Shaun Foulkes
 * SPDX-License-Identifier: MIT
 *
 * Public API shim — delegates to internal modules.
 * See include/zigbee_ota.h for the external API (unchanged).
 */

#include "zigbee_ota.h"
#include "ota_state.h"
#include "ota_zigbee_transport.h"
#include "esp_log.h"
#include "esp_zigbee_cluster.h"
#include "esp_zigbee_ota.h"

static const char *TAG = "zigbee_ota";

static bool s_initialized = false;

esp_err_t zigbee_ota_init(esp_zb_cluster_list_t *cluster_list, uint8_t endpoint,
                           const zigbee_ota_config_t *config)
{
    if (!cluster_list || !config) {
        ESP_LOGE(TAG, "Invalid arguments");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_initialized) {
        ESP_LOGW(TAG, "OTA already initialized");
        return ESP_OK;
    }

    ota_state_init(config, endpoint);

    /* Create OTA cluster */
    esp_zb_ota_cluster_cfg_t ota_cluster_cfg = {
        .ota_upgrade_file_version        = config->current_file_version,
        .ota_upgrade_downloaded_file_ver = ESP_ZB_ZCL_OTA_UPGRADE_DOWNLOADED_FILE_VERSION_DEF_VALUE,
        .ota_upgrade_manufacturer        = config->manufacturer_code,
        .ota_upgrade_image_type          = config->image_type,
    };

    esp_zb_attribute_list_t *ota_cluster = esp_zb_ota_cluster_create(&ota_cluster_cfg);
    if (!ota_cluster) {
        ESP_LOGE(TAG, "Failed to create OTA cluster");
        return ESP_ERR_NO_MEM;
    }

    esp_zb_zcl_ota_upgrade_client_variable_t variable_config = {
        .timer_query   = config->query_interval_minutes,
        .hw_version    = config->hw_version,
        .max_data_size = config->max_data_size,
    };

    uint16_t ota_server_addr = 0xffff;
    uint8_t  ota_server_ep   = 0xff;

    esp_err_t ret;
    ret = esp_zb_ota_cluster_add_attr(ota_cluster,
              ESP_ZB_ZCL_ATTR_OTA_UPGRADE_CLIENT_DATA_ID, &variable_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add OTA client data attr: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_zb_ota_cluster_add_attr(ota_cluster,
              ESP_ZB_ZCL_ATTR_OTA_UPGRADE_SERVER_ADDR_ID, &ota_server_addr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add OTA server addr attr: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_zb_ota_cluster_add_attr(ota_cluster,
              ESP_ZB_ZCL_ATTR_OTA_UPGRADE_SERVER_ENDPOINT_ID, &ota_server_ep);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add OTA server endpoint attr: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_zb_cluster_list_add_ota_cluster(cluster_list, ota_cluster,
                                               ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add OTA cluster to list: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "OTA client initialised on endpoint %d", endpoint);
    ESP_LOGI(TAG, "  Manufacturer: 0x%04x", config->manufacturer_code);
    ESP_LOGI(TAG, "  Image type:   0x%04x", config->image_type);
    ESP_LOGI(TAG, "  Version:      0x%08lx", config->current_file_version);
    ESP_LOGI(TAG, "  Query interval: %u min", config->query_interval_minutes);
    return ESP_OK;
}

esp_err_t zigbee_ota_register_status_callback(zigbee_ota_status_callback_t callback)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "OTA not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    ota_state_set_callback(callback);
    ESP_LOGI(TAG, "Status callback %s", callback ? "registered" : "unregistered");
    return ESP_OK;
}

esp_err_t zigbee_ota_action_handler(esp_zb_core_action_callback_id_t callback_id,
                                     const void *message)
{
    if (!s_initialized) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    switch (callback_id) {
    case ESP_ZB_CORE_OTA_UPGRADE_VALUE_CB_ID:
        return ota_zigbee_handle_upgrade_value(
            *(esp_zb_zcl_ota_upgrade_value_message_t *)message);

    case ESP_ZB_CORE_OTA_UPGRADE_QUERY_IMAGE_RESP_CB_ID:
        return ota_zigbee_handle_query_image_resp(
            *(esp_zb_zcl_ota_upgrade_query_image_resp_message_t *)message);

    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t zigbee_ota_start_query(uint16_t server_addr, uint8_t server_endpoint)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "OTA not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "Querying OTA server 0x%04x ep %u", server_addr, server_endpoint);
    esp_err_t ret = esp_zb_ota_upgrade_client_query_image_req(server_addr, server_endpoint);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Query failed: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t zigbee_ota_set_query_interval(uint16_t interval_minutes)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "OTA not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = esp_zb_ota_upgrade_client_query_interval_set(
        ota_state_get_endpoint(), interval_minutes);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set query interval failed: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Query interval set to %u min", interval_minutes);
    return ESP_OK;
}

#if CONFIG_IDF_TARGET_ESP32C6
#include "ota_trigger_web.h"

esp_err_t zigbee_ota_start_wifi_update(const char *index_url)
{
    return ota_trigger_web_start(index_url);
}
#endif /* CONFIG_IDF_TARGET_ESP32C6 */
