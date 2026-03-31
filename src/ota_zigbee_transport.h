/*
 * SPDX-FileCopyrightText: 2026 Shaun Foulkes
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "esp_err.h"
#include "esp_zigbee_ota.h"
#include "esp_zigbee_core.h"

/**
 * @brief Handle Zigbee OTA upgrade value callbacks (START / RECEIVE / CHECK / FINISH / ABORT)
 *
 * Called from zigbee_ota_action_handler() for ESP_ZB_CORE_OTA_UPGRADE_VALUE_CB_ID.
 */
esp_err_t ota_zigbee_handle_upgrade_value(esp_zb_zcl_ota_upgrade_value_message_t msg);

/**
 * @brief Handle Zigbee OTA query image response
 *
 * Called from zigbee_ota_action_handler() for ESP_ZB_CORE_OTA_UPGRADE_QUERY_IMAGE_RESP_CB_ID.
 * Extracts server address and URL then delegates to ota_trigger_z2m for transport selection.
 */
esp_err_t ota_zigbee_handle_query_image_resp(esp_zb_zcl_ota_upgrade_query_image_resp_message_t msg);

/**
 * @brief Deferred retry alarm callback
 *
 * Registered with esp_zb_scheduler_alarm() after an OTA ABORT.
 * Re-sends Query Next Image Request to the stored server.
 */
void ota_zigbee_retry_alarm(uint8_t param);

/**
 * @brief Send Zigbee Upgrade End Request to the stored OTA server
 *
 * Used by ota_wifi_transport after a Wi-Fi download completes (or fails) to inform
 * Z2M that the OTA is done. Z2M re-interviews the device on receipt and reads the
 * updated sw_build_id to confirm the version.
 *
 * @param status  ESP_ZB_ZCL_STATUS_SUCCESS on success, any error code on failure
 */
void ota_zigbee_send_upgrade_end(esp_zb_zcl_status_t status);
