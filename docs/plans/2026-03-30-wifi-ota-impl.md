# Wi-Fi OTA Transport Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement Wi-Fi OTA firmware download for the ESP32-C6 target of the LD2450 sensor — fetches an OTA index JSON, resolves the device-specific `.ota` URL, and installs it via existing `ota_writer`/`ota_header` modules.

**Architecture:** C6-only code already conditionally compiled by CMakeLists. The new implementation fills three stubs (`ota_wifi_transport.c`, `ota_trigger_z2m.c` Wi-Fi path, and a new web API endpoint in the LD2450 app) and adds a thin public-API wrapper in `zigbee_ota.c`. No changes to public `zigbee_ota.h` types.

**Tech Stack:** ESP-IDF 5.5+, `esp_http_client`, `cJSON` (`json` component), `esp_wifi`, `esp_crt_bundle` (HTTPS). Target: ESP32-C6 only. H2 build remains unaffected.

---

## Repo Paths

| Repo | Git dir |
|------|---------|
| zigbee-ota component (worktree) | `/data/shaun/Nextcloud/coding/src/esp32-zigbee-ota/esp32_zigbee_ota/.worktrees/modular-ota/` |
| LD2450 app | `/data/shaun/Nextcloud/coding/src/ld2450-zb-h2/ld2450_zb_h2/` |

All `git` commands for the OTA component run from the worktree dir. All `idf.py` commands run from the LD2450 repo dir.

---

## Hardcoded Default Index URL

```
https://shaunpccom.github.io/zigbee-ota-index/ota_index.json
```

The index JSON is an array of objects with `manufacturerCode`, `imageType`, `fileVersion`, and `url` fields. The device finds its entry by matching `imageType` from `ota_state_get_config()->image_type`.

---

### Task 1: CMakeLists — add `esp_wifi` and `json` to C6 REQUIRES

**Files:**
- Modify: `CMakeLists.txt` (component)

**Step 1: Open the file and verify current state**

Read `CMakeLists.txt`. Confirm the C6 block currently only appends `esp_http_client`.

**Step 2: Add the two missing dependencies**

Change:
```cmake
    list(APPEND requires esp_http_client)
```
To:
```cmake
    list(APPEND requires esp_http_client esp_wifi json)
```

`esp_wifi` is needed for `esp_wifi_sta_get_ap_info()`. `json` is the IDF component that provides `cJSON.h`.

**Step 3: Build H2 to confirm no regression**

```bash
cd /data/shaun/Nextcloud/coding/src/ld2450-zb-h2/ld2450_zb_h2
idf.py build 2>&1 | tail -5
```

Expected: `Project build complete.` (H2 target is unaffected — C6 block not compiled)

**Step 4: Commit**

```bash
cd /data/shaun/Nextcloud/coding/src/esp32-zigbee-ota/esp32_zigbee_ota/.worktrees/modular-ota
git add CMakeLists.txt
git commit -m "build(c6): add esp_wifi and json to C6 REQUIRES"
```

---

### Task 2: `ota_wifi_transport.h` — add default index URL constant

**Files:**
- Modify: `src/ota_wifi_transport.h`

**Step 1: Update the header**

Add the constant and update the `ota_wifi_transport_start` doc to reflect the new signature (accepts index URL, not direct .ota URL):

```c
/*
 * SPDX-FileCopyrightText: 2026 Shaun Foulkes
 * SPDX-License-Identifier: MIT
 */

#pragma once

/* Wi-Fi OTA transport — ESP32-C6 only */
#ifdef CONFIG_IDF_TARGET_ESP32C6

#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief Default OTA index URL
 *
 * GitHub Pages aggregated index. Contains one entry per device, keyed by
 * (manufacturerCode, imageType). The Wi-Fi transport fetches this, finds the
 * entry matching the running device's imageType, and downloads that .ota file.
 */
#define OTA_WIFI_DEFAULT_INDEX_URL \
    "https://shaunpccom.github.io/zigbee-ota-index/ota_index.json"

/**
 * @brief Check whether a Wi-Fi station connection is active
 *
 * Wraps esp_wifi_sta_get_ap_info() so callers (e.g. ota_trigger_z2m) do not
 * need to include esp_wifi.h directly.
 *
 * @return true if Wi-Fi STA is associated with an AP, false otherwise
 */
bool ota_wifi_transport_is_connected(void);

/**
 * @brief Start a Wi-Fi OTA download in a background task
 *
 * Fetches the OTA index from @p index_url, resolves the device's .ota file URL
 * by matching imageType, then downloads and installs the firmware.
 *
 * The OTA in-progress slot must already be acquired by the caller before calling
 * this. The URL string is copied internally — no lifetime constraint on the caller.
 *
 * @param index_url  OTA index JSON URL, or NULL to use OTA_WIFI_DEFAULT_INDEX_URL.
 *
 * @return ESP_OK         Task spawned successfully (download in background)
 * @return ESP_ERR_NO_MEM strdup or task creation failed
 */
esp_err_t ota_wifi_transport_start(const char *index_url);

#endif /* CONFIG_IDF_TARGET_ESP32C6 */
```

**Step 2: Commit**

```bash
git add src/ota_wifi_transport.h
git commit -m "feat(wifi-ota): add OTA_WIFI_DEFAULT_INDEX_URL constant, update header"
```

---

### Task 3: `ota_wifi_transport.c` — implement `is_connected` and `ota_index_resolve`

**Files:**
- Modify: `src/ota_wifi_transport.c`

This task adds the Wi-Fi connection check and the index-fetching helper. The download loop comes in the next task to keep each commit reviewable.

**Step 1: Replace the file content with this skeleton + first two implementations**

```c
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
        .url              = index_url,
        .timeout_ms       = 15000,
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
/*  Stubs for next task (download loop)                                 */
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
```

**Step 2: Build C6 target to confirm it compiles**

```bash
cd /data/shaun/Nextcloud/coding/src/ld2450-zb-h2/ld2450_zb_h2
idf.py set-target esp32c6
idf.py build 2>&1 | grep -E "error:|warning:|build complete"
```

Expected: no errors. There will be warnings about `ota_index_resolve` being defined but not used yet — acceptable at this stage.

**Step 3: Commit**

```bash
cd /data/shaun/Nextcloud/coding/src/esp32-zigbee-ota/esp32_zigbee_ota/.worktrees/modular-ota
git add src/ota_wifi_transport.c
git commit -m "feat(wifi-ota): implement is_connected and ota_index_resolve"
```

---

### Task 4: `ota_wifi_transport.c` — implement download loop and restart

**Files:**
- Modify: `src/ota_wifi_transport.c`

Replace the `wifi_ota_task` stub and the `ota_wifi_transport_start` function with the full implementation. Do not change `ota_wifi_transport_is_connected` or `ota_index_resolve`.

**Step 1: Add the restart callback and replace `wifi_ota_task`**

Replace the `/* Stubs for next task */` section with this:

```c
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
/*  Download loop                                                        */
/* ------------------------------------------------------------------ */

static uint8_t s_dl_buf[4096];  /* static — not on task stack */

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

    /* --- 3. HTTP GET the .ota file --- */
    esp_http_client_config_t http_cfg = {
        .url               = ota_url,
        .timeout_ms        = 60000,
        .buffer_size       = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .follow_redirects  = true,
        .max_redirection_count = 5,
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
                                     8192, url_copy, 5, NULL);
    if (created != pdPASS) {
        free(url_copy);
        ESP_LOGE(TAG, "Task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
```

**Step 2: Build C6**

```bash
cd /data/shaun/Nextcloud/coding/src/ld2450-zb-h2/ld2450_zb_h2
idf.py build 2>&1 | grep -E "error:|warning:|build complete"
```

Expected: `Project build complete.` No errors.

**Step 3: Build H2 — confirm no regression**

```bash
idf.py set-target esp32h2
idf.py build 2>&1 | tail -5
```

Expected: `Project build complete.`

**Step 4: Commit**

```bash
cd /data/shaun/Nextcloud/coding/src/esp32-zigbee-ota/esp32_zigbee_ota/.worktrees/modular-ota
git add src/ota_wifi_transport.c
git commit -m "feat(wifi-ota): implement download loop, header strip, partition write, restart"
```

---

### Task 5: `ota_trigger_z2m.c` — enable Wi-Fi path unconditionally when connected

**Files:**
- Modify: `src/ota_trigger_z2m.c`

Currently, the Wi-Fi branch requires `wifi_url != NULL`, which is always false (no URL in the SDK struct). Remove that guard so the device tries Wi-Fi whenever it is connected.

**Step 1: Replace the C6 block**

Current:
```c
#if CONFIG_IDF_TARGET_ESP32C6
    /* On C6: prefer Wi-Fi transport when a URL is available and Wi-Fi is up */
    if (wifi_url != NULL) {
        if (ota_wifi_transport_is_connected()) {
            esp_err_t ret = ota_state_acquire(OTA_SOURCE_WIFI);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "OTA already in progress, ignoring Z2M notification");
                return ret;
            }
            ESP_LOGI(TAG, "Wi-Fi connected — using Wi-Fi OTA transport");
            ESP_LOGI(TAG, "  URL: %s", wifi_url);
            return ota_wifi_transport_start(wifi_url);
        }

        ESP_LOGW(TAG, "Wi-Fi URL provided but not connected — falling back to Zigbee");
    }
#endif /* CONFIG_IDF_TARGET_ESP32C6 */
```

Replace with:
```c
#if CONFIG_IDF_TARGET_ESP32C6
    /* On C6: prefer Wi-Fi transport when a station connection is active.
     * wifi_url from the Z2M query response is always NULL (field not present in SDK).
     * The Wi-Fi transport resolves the .ota URL from its built-in index. */
    if (ota_wifi_transport_is_connected()) {
        esp_err_t ret = ota_state_acquire(OTA_SOURCE_WIFI);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "OTA already in progress, ignoring Z2M notification");
            return ret;
        }
        ESP_LOGI(TAG, "Wi-Fi connected — using Wi-Fi OTA transport");
        return ota_wifi_transport_start(NULL);  /* NULL = use OTA_WIFI_DEFAULT_INDEX_URL */
    }
    ESP_LOGI(TAG, "Wi-Fi not connected — falling back to Zigbee OTA");
#endif /* CONFIG_IDF_TARGET_ESP32C6 */
```

The `wifi_url` parameter is now unused on C6. It can remain in the signature (the Z2M SDK may eventually provide it) but no longer gates the Wi-Fi path.

**Step 2: Build C6 and H2**

```bash
cd /data/shaun/Nextcloud/coding/src/ld2450-zb-h2/ld2450_zb_h2
idf.py set-target esp32c6 && idf.py build 2>&1 | tail -3
idf.py set-target esp32h2 && idf.py build 2>&1 | tail -3
```

Expected: both complete without errors.

**Step 3: Commit**

```bash
cd /data/shaun/Nextcloud/coding/src/esp32-zigbee-ota/esp32_zigbee_ota/.worktrees/modular-ota
git add src/ota_trigger_z2m.c
git commit -m "feat(wifi-ota): enable Wi-Fi path from Z2M trigger when connected"
```

---

### Task 6: Public API — add `zigbee_ota_start_wifi_update()` wrapper

**Files:**
- Modify: `include/zigbee_ota.h`
- Modify: `src/zigbee_ota.c`

The LD2450 `web_server.c` is a consumer of the `zigbee_ota` component and can only see `include/zigbee_ota.h`. `ota_trigger_web.h` is in `src/` (PRIV_INCLUDE_DIRS) and is inaccessible to consumers. A thin public wrapper gives the app a clean entry point.

**Step 1: Add the declaration to `include/zigbee_ota.h`**

Add before the final `#ifdef __cplusplus` / `#endif` block:

```c
#if CONFIG_IDF_TARGET_ESP32C6
/**
 * @brief Trigger a Wi-Fi OTA update from the web UI (C6 only)
 *
 * Acquires the OTA slot and starts a background download task. Returns
 * immediately — the download runs asynchronously.
 *
 * @param index_url  OTA index JSON URL, or NULL to use the built-in default.
 *                   The string is copied internally.
 *
 * @return ESP_OK               Download started (HTTP 202)
 * @return ESP_ERR_INVALID_STATE OTA already in progress (HTTP 409)
 * @return ESP_ERR_NO_MEM       Memory or task creation failure
 */
esp_err_t zigbee_ota_start_wifi_update(const char *index_url);
#endif /* CONFIG_IDF_TARGET_ESP32C6 */
```

**Step 2: Add the implementation to `src/zigbee_ota.c`**

Find the end of `zigbee_ota.c` and append:

```c
#if CONFIG_IDF_TARGET_ESP32C6
#include "ota_trigger_web.h"

esp_err_t zigbee_ota_start_wifi_update(const char *index_url)
{
    return ota_trigger_web_start(index_url);
}
#endif /* CONFIG_IDF_TARGET_ESP32C6 */
```

**Step 3: Build C6 and H2**

```bash
cd /data/shaun/Nextcloud/coding/src/ld2450-zb-h2/ld2450_zb_h2
idf.py set-target esp32c6 && idf.py build 2>&1 | tail -3
idf.py set-target esp32h2 && idf.py build 2>&1 | tail -3
```

**Step 4: Commit**

```bash
cd /data/shaun/Nextcloud/coding/src/esp32-zigbee-ota/esp32_zigbee_ota/.worktrees/modular-ota
git add include/zigbee_ota.h src/zigbee_ota.c
git commit -m "feat(wifi-ota): add zigbee_ota_start_wifi_update() public API wrapper"
```

---

### Task 7: LD2450 `web_server.c` — add `POST /api/ota` handler

**Files:**
- Modify: `/data/shaun/Nextcloud/coding/src/ld2450-zb-h2/ld2450_zb_h2/main/web_server.c`

`web_server.c` is already compiled for C6 only (see `main/CMakeLists.txt`), so no `#ifdef` guards are needed within the file.

**Step 1: Add the include near the top of the file (after existing includes)**

```c
#include "zigbee_ota.h"
```

**Step 2: Add the handler function before `web_server_start()`**

Find the `handle_not_found` function (around line 751) and add the new handler just before it:

```c
/* ================================================================== */
/*  POST /api/ota — trigger Wi-Fi firmware update                      */
/* ================================================================== */

static esp_err_t handle_post_ota(httpd_req_t *req)
{
    /* Optional JSON body: { "url": "https://..." } to override the default index.
     * No body = use built-in OTA index URL. */
    char *body = read_body(req);
    const char *url = NULL;
    cJSON *root = NULL;

    if (body) {
        root = cJSON_Parse(body);
        if (root) {
            cJSON *url_j = cJSON_GetObjectItemCaseSensitive(root, "url");
            if (cJSON_IsString(url_j) && url_j->valuestring[0] != '\0') {
                url = url_j->valuestring;  /* valid until cJSON_Delete(root) */
            }
        }
    }

    esp_err_t ret = zigbee_ota_start_wifi_update(url);  /* copies url internally */

    cJSON_Delete(root);
    free(body);

    if (ret == ESP_OK) {
        httpd_resp_set_status(req, "202 Accepted");
    } else if (ret == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
    }

    cJSON *resp = cJSON_CreateObject();
    if (ret == ESP_OK) {
        cJSON_AddStringToObject(resp, "status", "update_started");
    } else {
        cJSON_AddStringToObject(resp, "error",
            ret == ESP_ERR_INVALID_STATE ? "OTA already in progress"
                                         : "Failed to start OTA");
    }
    send_json(req, 200, resp);  /* status already set above; 200 here = no override */
    cJSON_Delete(resp);
    return ESP_OK;
}
```

**Step 3: Register the handler**

In `web_server_start()`, add the new URI to the `uris[]` static array:

```c
        { .uri = "/api/ota",           .method = HTTP_POST, .handler = handle_post_ota       },
```

The `uris[]` array currently has 14 entries; `cfg.max_uri_handlers` is already 16. Adding one brings the total (including WS) to 16, exactly at the limit — no change to `max_uri_handlers` needed.

**Step 4: Build C6**

This is the first build that exercises the full chain (LD2450 app → zigbee_ota public API → ota_trigger_web → ota_wifi_transport). If the linker cannot find `zigbee_ota`, add it to `C6_PRIV_REQUIRES` in `main/CMakeLists.txt`:

```cmake
set(C6_PRIV_REQUIRES esp_wifi esp_netif esp_event esp_http_server espressif__mdns spiffs zigbee_ota)
```

Run build:

```bash
cd /data/shaun/Nextcloud/coding/src/ld2450-zb-h2/ld2450_zb_h2
idf.py set-target esp32c6 && idf.py build 2>&1 | tail -5
```

Expected: `Project build complete.`

**Step 5: Build H2 — confirm no regression**

```bash
idf.py set-target esp32h2 && idf.py build 2>&1 | tail -5
```

Expected: `Project build complete.`

**Step 6: Commit OTA component changes**

```bash
cd /data/shaun/Nextcloud/coding/src/esp32-zigbee-ota/esp32_zigbee_ota/.worktrees/modular-ota
git status   # should be clean — all previous tasks committed
```

**Step 7: Commit LD2450 changes**

```bash
cd /data/shaun/Nextcloud/coding/src/ld2450-zb-h2/ld2450_zb_h2
git add main/web_server.c main/CMakeLists.txt
git commit -m "feat(c6): add POST /api/ota endpoint for Wi-Fi firmware update"
```

---

### Task 8: Functional verification

**Step 1: Flash C6 target and monitor**

```bash
cd /data/shaun/Nextcloud/coding/src/ld2450-zb-h2/ld2450_zb_h2
idf.py set-target esp32c6
idf.py -p /dev/ttyACM0 flash monitor
```

**Step 2: Verify Wi-Fi OTA via web endpoint**

The C6 device must be connected to Wi-Fi (use existing credentials). Find its IP from the monitor output or router.

```bash
# Replace 192.168.x.x with device IP
curl -s -X POST http://192.168.x.x/api/ota \
     -H "Content-Type: application/json" \
     -d '{}' -w "\nHTTP %{http_code}\n"
```

Expected: `HTTP 202` and `{"status":"update_started"}` in response.
Expected in device monitor: `Spawning Wi-Fi OTA task`, `Resolved OTA URL: https://github.com/...`, download progress logs, `Wi-Fi OTA complete — restarting in 3 s`.

**Step 3: Verify version update in Z2M**

After the device reboots and rejoins, Z2M should show the updated firmware version within ~5 minutes (next Query Image Request cycle). No manual action required.

**Step 4: Verify duplicate-trigger rejection**

While an OTA is in progress, send a second POST:
```bash
curl -s -X POST http://192.168.x.x/api/ota -H "Content-Type: application/json" -d '{}'
```
Expected: `HTTP 409` and `{"error":"OTA already in progress"}`.

**Step 5: Verify Z2M-triggered path (C6 with Wi-Fi connected)**

Trigger OTA from Z2M ("Update firmware" button). Monitor should show:
`Wi-Fi connected — using Wi-Fi OTA transport` (not Zigbee block transfer logs).

**Step 6: Verify H2 still uses Zigbee OTA**

Flash H2, trigger OTA from Z2M. Should see normal Zigbee block transfer logs — no Wi-Fi path involved.

---

## Summary of Files Changed

| Repo | File | Change |
|------|------|--------|
| zigbee-ota | `CMakeLists.txt` | `+esp_wifi +json` to C6 REQUIRES |
| zigbee-ota | `src/ota_wifi_transport.h` | `OTA_WIFI_DEFAULT_INDEX_URL`, updated docs |
| zigbee-ota | `src/ota_wifi_transport.c` | Full implementation |
| zigbee-ota | `src/ota_trigger_z2m.c` | Remove `wifi_url != NULL` guard |
| zigbee-ota | `include/zigbee_ota.h` | `zigbee_ota_start_wifi_update()` declaration |
| zigbee-ota | `src/zigbee_ota.c` | `zigbee_ota_start_wifi_update()` implementation |
| ld2450 | `main/web_server.c` | `handle_post_ota()` + `/api/ota` registration |
| ld2450 | `main/CMakeLists.txt` | `zigbee_ota` to C6_PRIV_REQUIRES if needed |
