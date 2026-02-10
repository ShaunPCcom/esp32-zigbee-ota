/*
 * SPDX-FileCopyrightText: 2026 Shaun Foulkes
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include "esp_zigbee_type.h"

/**
 * @brief OTA upgrade status events
 *
 * These status codes are passed to the application callback during OTA operations.
 */
typedef enum {
    ZIGBEE_OTA_STATUS_START = 0,        /*!< OTA upgrade started */
    ZIGBEE_OTA_STATUS_DOWNLOADING,      /*!< Downloading image blocks */
    ZIGBEE_OTA_STATUS_DOWNLOAD_COMPLETE,/*!< Download finished, validating */
    ZIGBEE_OTA_STATUS_APPLYING,         /*!< Applying new firmware */
    ZIGBEE_OTA_STATUS_SUCCESS,          /*!< OTA completed successfully, will restart */
    ZIGBEE_OTA_STATUS_FAILED,           /*!< OTA failed or aborted */
    ZIGBEE_OTA_STATUS_SERVER_NOT_FOUND, /*!< OTA server not found on network */
} zigbee_ota_status_t;

/**
 * @brief OTA status callback function type
 *
 * Called by the OTA component during various stages of the upgrade process.
 * Use this for application-level feedback (e.g., LED status indication).
 *
 * @param status Current OTA status
 * @param progress_percent Download progress (0-100), only valid during DOWNLOADING status
 */
typedef void (*zigbee_ota_status_callback_t)(zigbee_ota_status_t status, uint8_t progress_percent);

/**
 * @brief OTA client configuration
 */
typedef struct {
    uint16_t manufacturer_code;         /*!< Manufacturer code (e.g., 0x131B for Espressif) */
    uint16_t image_type;                /*!< Application-specific image type identifier */
    uint32_t current_file_version;      /*!< Current running firmware version */
    uint16_t hw_version;                /*!< Hardware version */
    uint16_t query_interval_minutes;    /*!< Auto-query interval in minutes (0 = disabled, default 1440 = 24 hours) */
    uint8_t max_data_size;              /*!< Maximum OTA block size in bytes (default 64) */
} zigbee_ota_config_t;

/**
 * @brief Default OTA configuration values
 */
#define ZIGBEE_OTA_CONFIG_DEFAULT() {                           \
    .manufacturer_code = 0x131B,        /* Espressif */         \
    .image_type = 0x0001,               /* Application default */\
    .current_file_version = 0x00000001, /* Version 1.0.0.1 */   \
    .hw_version = 1,                                            \
    .query_interval_minutes = 1440,     /* 24 hours */          \
    .max_data_size = 64,                                        \
}

/**
 * @brief Initialize Zigbee OTA client
 *
 * Adds the OTA upgrade cluster (0x0019) to the specified endpoint and configures
 * the OTA client with the provided settings. Must be called during endpoint creation,
 * before esp_zb_device_register().
 *
 * @param[in] cluster_list Cluster list to add OTA cluster to
 * @param[in] endpoint Endpoint number where OTA cluster will reside
 * @param[in] config OTA client configuration
 *
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_INVALID_ARG: Invalid arguments
 *      - ESP_ERR_NO_MEM: Out of memory
 */
esp_err_t zigbee_ota_init(esp_zb_cluster_list_t *cluster_list, uint8_t endpoint, const zigbee_ota_config_t *config);

/**
 * @brief Register OTA status callback
 *
 * Optional callback for receiving OTA status updates. Use this to provide user
 * feedback (e.g., LED color changes during download, display progress, etc.).
 *
 * Must be called AFTER zigbee_ota_init() and BEFORE esp_zb_start().
 *
 * @param[in] callback Status callback function (NULL to unregister)
 *
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_INVALID_STATE: OTA not initialized
 */
esp_err_t zigbee_ota_register_status_callback(zigbee_ota_status_callback_t callback);

/**
 * @brief Register the OTA action handler
 *
 * Internal function that must be called by the application's action handler.
 * This allows the OTA component to receive Zigbee action callbacks.
 *
 * Call this from your esp_zb_core_action_handler_register() callback:
 *
 * @code
 * esp_err_t my_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message) {
 *     // Let OTA component handle OTA-related callbacks
 *     esp_err_t ret = zigbee_ota_action_handler(callback_id, message);
 *     if (ret != ESP_ERR_NOT_SUPPORTED) {
 *         return ret;  // OTA component handled it
 *     }
 *
 *     // Handle other callbacks here
 *     switch (callback_id) {
 *         // ... your application callbacks
 *     }
 *     return ESP_OK;
 * }
 * @endcode
 *
 * @param[in] callback_id Zigbee action callback ID
 * @param[in] message Callback message data
 *
 * @return
 *      - ESP_OK: OTA callback handled successfully
 *      - ESP_ERR_NOT_SUPPORTED: Not an OTA callback (application should handle)
 *      - Other: Error during OTA callback processing
 */
esp_err_t zigbee_ota_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message);

/**
 * @brief Manually trigger OTA image query
 *
 * Sends an immediate query to the specified OTA server to check for available updates.
 * Normally not needed as the component automatically queries based on query_interval_minutes.
 *
 * @param[in] server_addr Short address of the OTA server (typically 0x0000 for coordinator)
 * @param[in] server_endpoint Endpoint of the OTA server
 *
 * @return
 *      - ESP_OK: Query sent successfully
 *      - ESP_ERR_INVALID_STATE: OTA not initialized or device not joined to network
 *      - ESP_FAIL: Failed to send query
 */
esp_err_t zigbee_ota_start_query(uint16_t server_addr, uint8_t server_endpoint);

/**
 * @brief Set OTA query interval
 *
 * Changes the automatic query interval. Set to 0 to disable automatic queries.
 *
 * @param[in] interval_minutes Query interval in minutes (0 = disabled)
 *
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_INVALID_STATE: OTA not initialized
 *      - ESP_FAIL: Failed to update interval
 */
esp_err_t zigbee_ota_set_query_interval(uint16_t interval_minutes);

#ifdef __cplusplus
}
#endif
