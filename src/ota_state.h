/*
 * SPDX-FileCopyrightText: 2026 Shaun Foulkes
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "zigbee_ota.h"

/**
 * @brief OTA source — which transport is currently active
 */
typedef enum {
    OTA_SOURCE_NONE    = 0,
    OTA_SOURCE_ZIGBEE  = 1,
    OTA_SOURCE_WIFI    = 2,
} ota_source_t;

/**
 * @brief Initialise shared OTA state. Must be called once before any other ota_state_* call.
 *
 * @param config  OTA configuration (copied internally)
 * @param endpoint Zigbee endpoint hosting the OTA cluster (for query interval adjustment)
 */
void ota_state_init(const zigbee_ota_config_t *config, uint8_t endpoint);

/** Register / unregister status callback */
void ota_state_set_callback(zigbee_ota_status_callback_t cb);

/** Returns true if an OTA download is currently in progress */
bool ota_state_is_in_progress(void);

/** Returns the source that currently holds the OTA slot (OTA_SOURCE_NONE if idle) */
ota_source_t ota_state_get_source(void);

/**
 * @brief Atomically claim the OTA slot for a given source.
 *
 * Uses taskENTER_CRITICAL to prevent a race between Z2M notification and Web UI trigger.
 *
 * @return ESP_OK            Slot acquired.
 * @return ESP_ERR_INVALID_STATE  Already in progress (from any source).
 */
esp_err_t ota_state_acquire(ota_source_t source);

/** Release the OTA slot. Call after completion or failure. */
void ota_state_release(void);

/**
 * @brief Invoke status callback (no-op if none registered)
 */
void ota_state_notify(zigbee_ota_status_t status, uint8_t progress_pct);

/* --- Server address (stored on query image response; used by retry + upgrade end) --- */

void      ota_state_set_server(uint16_t addr, uint8_t endpoint);
uint16_t  ota_state_get_server_addr(void);
uint8_t   ota_state_get_server_endpoint(void);

/* --- Abort retry counter --- */

uint8_t   ota_state_get_abort_retries(void);
void      ota_state_inc_abort_retries(void);
void      ota_state_reset_abort_retries(void);

/* --- Config / endpoint accessors --- */

const zigbee_ota_config_t *ota_state_get_config(void);
uint8_t   ota_state_get_endpoint(void);

/* --- Wi-Fi OTA index URL override (C6 only; NULL means use built-in default) --- */

void        ota_state_set_wifi_index_url(const char *url);  /* NULL clears override */
const char *ota_state_get_wifi_index_url(void);             /* NULL if no override set */
