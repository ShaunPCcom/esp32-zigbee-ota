/*
 * SPDX-FileCopyrightText: 2026 Shaun Foulkes
 * SPDX-License-Identifier: MIT
 */

#include "ota_zigbee_transport.h"
#include "ota_state.h"
#include "ota_writer.h"
#include "ota_header.h"
#include "ota_trigger_z2m.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_zigbee_ota.h"

static const char *TAG = "ota_zb_transport";

/* Transfer-local state — reset at START, used through FINISH/CHECK */
static struct {
    uint32_t total_size;
    uint32_t downloaded_size;
    int64_t  start_time_us;
} s_xfer;

static void reset_xfer_state(void)
{
    s_xfer.total_size      = 0;
    s_xfer.downloaded_size = 0;
    s_xfer.start_time_us   = 0;
}

static uint8_t progress_pct(uint32_t downloaded, uint32_t total)
{
    if (total == 0) {
        return 0;
    }
    uint32_t pct = (downloaded * 100) / total;
    return (pct > 100) ? 100 : (uint8_t)pct;
}

/* =========================================================================
 * Deferred restart — scheduled via esp_zb_scheduler_alarm after FINISH.
 * Must be via Zigbee scheduler alarm (not esp_timer) to allow the stack to
 * complete the Upgrade End Response TX before we restart.
 * ========================================================================= */

static void ota_deferred_restart(uint8_t param)
{
    (void)param;
    ESP_LOGW(TAG, "Deferred OTA restart executing");
    esp_restart();
}

/* =========================================================================
 * ota_zigbee_retry_alarm — re-query after ABORT
 * ========================================================================= */

void ota_zigbee_retry_alarm(uint8_t param)
{
    (void)param;
    const zigbee_ota_config_t *cfg = ota_state_get_config();

    ESP_LOGW(TAG, "OTA retry %u/%u: querying 0x%04x ep %u",
             ota_state_get_abort_retries(), cfg->max_abort_retries,
             ota_state_get_server_addr(), ota_state_get_server_endpoint());

    esp_err_t ret = esp_zb_ota_upgrade_client_query_image_req(
        ota_state_get_server_addr(), ota_state_get_server_endpoint());

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Retry query failed: %s", esp_err_to_name(ret));
        ota_state_reset_abort_retries();
        ota_state_release();
        ota_state_notify(ZIGBEE_OTA_STATUS_FAILED, 0);
    }
}

/* =========================================================================
 * ota_zigbee_send_upgrade_end — called by Wi-Fi transport to close Z2M state
 * ========================================================================= */

void ota_zigbee_send_upgrade_end(esp_zb_zcl_status_t status)
{
    uint16_t server_addr = ota_state_get_server_addr();
    uint8_t  server_ep   = ota_state_get_server_endpoint();

    if (server_addr == 0xffff) {
        ESP_LOGW(TAG, "ota_zigbee_send_upgrade_end: server address unknown, skipping");
        return;
    }

    ESP_LOGI(TAG, "Sending Upgrade End Request to 0x%04x ep %u (status 0x%02x)",
             server_addr, server_ep, (unsigned)status);

    /* TODO: determine correct ESP Zigbee SDK function for sending a standalone
     * Upgrade End Request (not part of the automatic Zigbee block transfer state
     * machine). Candidate: esp_zb_ota_upgrade_client_send_upgrade_end_req() or
     * equivalent. Implement when adding Wi-Fi OTA transport. */
    ESP_LOGW(TAG, "ota_zigbee_send_upgrade_end: not yet implemented (stub)");
}

/* =========================================================================
 * Upgrade value handler — START / RECEIVE / APPLY / CHECK / FINISH / ABORT
 * ========================================================================= */

esp_err_t ota_zigbee_handle_upgrade_value(esp_zb_zcl_ota_upgrade_value_message_t msg)
{
    esp_err_t ret = ESP_OK;

    if (msg.info.status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "OTA value callback with ZCL error 0x%x", msg.info.status);
        return ESP_FAIL;
    }

    switch (msg.upgrade_status) {

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_START:
        /* Defensive guard: the Zigbee SDK may fire START even if the query
         * response callback returned an error (e.g. Wi-Fi OTA already running).
         * Only proceed if the Zigbee transport legitimately holds the slot. */
        if (ota_state_get_source() != OTA_SOURCE_ZIGBEE) {
            ESP_LOGW(TAG, "Zigbee OTA START rejected — slot held by source %d (not Zigbee)",
                     (int)ota_state_get_source());
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "OTA Zigbee transfer started");
        reset_xfer_state();
        s_xfer.start_time_us = esp_timer_get_time();
        ota_header_reset(OTA_HEADER_MODE_ZIGBEE);

        ret = ota_writer_begin();
        if (ret != ESP_OK) {
            ota_state_release();
            ota_state_notify(ZIGBEE_OTA_STATUS_FAILED, 0);
            return ret;
        }
        ota_state_notify(ZIGBEE_OTA_STATUS_START, 0);
        break;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_RECEIVE:
        s_xfer.total_size      = msg.ota_header.image_size;
        s_xfer.downloaded_size += msg.payload_size;

        if (msg.payload_size && msg.payload) {
            const uint8_t *fw_data;
            size_t         fw_len;
            ota_header_process((const uint8_t *)msg.payload, msg.payload_size,
                               &fw_data, &fw_len);
            if (fw_len > 0) {
                ret = ota_writer_write(fw_data, fw_len);
                if (ret != ESP_OK) {
                    ota_writer_abort();
                    ota_state_release();
                    ota_state_notify(ZIGBEE_OTA_STATUS_FAILED, 0);
                    return ret;
                }
            }
        }

        {
            uint8_t pct = progress_pct(s_xfer.downloaded_size, s_xfer.total_size);
            ESP_LOGI(TAG, "OTA progress: %u%% (%lu/%lu bytes)",
                     pct, s_xfer.downloaded_size, s_xfer.total_size);
            ota_state_notify(ZIGBEE_OTA_STATUS_DOWNLOADING, pct);
        }
        break;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_APPLY:
        ESP_LOGI(TAG, "OTA apply");
        ota_state_notify(ZIGBEE_OTA_STATUS_APPLYING, 100);
        break;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_CHECK:
        if (s_xfer.downloaded_size != s_xfer.total_size) {
            ESP_LOGE(TAG, "OTA incomplete: %lu/%lu bytes",
                     s_xfer.downloaded_size, s_xfer.total_size);
            ota_state_notify(ZIGBEE_OTA_STATUS_FAILED, 0);
            ret = ESP_FAIL;
        } else {
            ESP_LOGI(TAG, "OTA download complete — validation passed");
            ota_state_notify(ZIGBEE_OTA_STATUS_DOWNLOAD_COMPLETE, 100);
        }
        /* Reset counters; CHECK fires before FINISH */
        s_xfer.downloaded_size = 0;
        s_xfer.total_size      = 0;
        break;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_FINISH:
        {
            int64_t duration_ms = (esp_timer_get_time() - s_xfer.start_time_us) / 1000;
            ESP_LOGI(TAG, "OTA finished — version 0x%08lx, size %lu bytes, %lld ms",
                     msg.ota_header.file_version,
                     msg.ota_header.image_size,
                     duration_ms);

            ret = ota_writer_finish();
            if (ret != ESP_OK) {
                ota_state_release();
                ota_state_notify(ZIGBEE_OTA_STATUS_FAILED, 0);
                return ret;
            }

            ota_state_reset_abort_retries();
            ota_state_notify(ZIGBEE_OTA_STATUS_SUCCESS, 100);
            ota_state_release();

            /* Deferred restart: allow Zigbee stack to complete Upgrade End Response
             * TX and allow Z2M to read updated sw_build_id before we restart. */
            ESP_LOGW(TAG, "OTA complete, restart in 3 s");
            esp_zb_scheduler_alarm(ota_deferred_restart, 0, 3000);
        }
        break;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ABORT:
        ESP_LOGW(TAG, "OTA aborted");
        ota_writer_abort();
        s_xfer.downloaded_size = 0;
        s_xfer.total_size      = 0;

        {
            const zigbee_ota_config_t *cfg = ota_state_get_config();
            if (cfg->max_abort_retries > 0 &&
                ota_state_get_abort_retries() < cfg->max_abort_retries &&
                ota_state_get_server_addr() != 0xffff)
            {
                ota_state_inc_abort_retries();
                ESP_LOGW(TAG, "Scheduling retry %u/%u in %us",
                         ota_state_get_abort_retries(), cfg->max_abort_retries,
                         cfg->retry_delay_seconds);
                ota_state_notify(ZIGBEE_OTA_STATUS_RETRYING, 0);
                /* Keep in-progress guard held through the retry delay — prevents a
                 * parallel trigger from interfering between retries. */
                esp_zb_scheduler_alarm(ota_zigbee_retry_alarm, 0,
                                       (uint32_t)cfg->retry_delay_seconds * 1000);
            } else {
                if (ota_state_get_abort_retries() >= cfg->max_abort_retries) {
                    ESP_LOGE(TAG, "Max retries (%u) exhausted", cfg->max_abort_retries);
                }
                ota_state_reset_abort_retries();
                ota_state_release();
                ota_state_notify(ZIGBEE_OTA_STATUS_FAILED, 0);
            }
        }
        ret = ESP_FAIL;
        break;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_SERVER_NOT_FOUND:
        ESP_LOGW(TAG, "OTA server not found");
        ota_state_notify(ZIGBEE_OTA_STATUS_SERVER_NOT_FOUND, 0);
        ret = ESP_FAIL;
        break;

    default:
        ESP_LOGI(TAG, "OTA status %d (unhandled)", msg.upgrade_status);
        break;
    }

    return ret;
}

/* =========================================================================
 * Query image response — dispatches to ota_trigger_z2m for transport choice
 * ========================================================================= */

esp_err_t ota_zigbee_handle_query_image_resp(esp_zb_zcl_ota_upgrade_query_image_resp_message_t msg)
{
    if (msg.info.status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "No OTA update available (status 0x%x)", msg.info.status);
        return ESP_OK;  /* Not an error — simply no update */
    }

    ESP_LOGI(TAG, "OTA image available: version 0x%08lx, size %lu, server 0x%04x ep %u",
             msg.file_version, msg.image_size,
             msg.server_addr.u.short_addr, msg.server_endpoint);

    /* Delegate transport selection to ota_trigger_z2m.
     * wifi_url is always NULL here — the ota_upgrade_url field does not exist
     * in the current ESP Zigbee SDK struct. URL extraction will be added when
     * Wi-Fi OTA transport is implemented (requires SDK research). */
    const char *wifi_url = NULL;

    return ota_trigger_z2m_on_image_available(
        msg.server_addr.u.short_addr,
        msg.server_endpoint,
        wifi_url);
}
