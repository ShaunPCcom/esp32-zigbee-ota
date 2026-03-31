# Wi-Fi OTA Transport Design

**Date:** 2026-03-30
**Branch:** `feat/modular-ota` (after modular refactor merges)
**Scope:** `esp32-zigbee-ota` component + `ld2450-zb-h2` firmware (C6 target only)

---

## Overview

Wi-Fi OTA allows the ESP32-C6 variant to download firmware directly over HTTP rather than
receiving it block-by-block via Zigbee. This is faster (minutes vs. tens of minutes) and
independent of Z2M's OTA pipeline.

The modular refactor (merged in the prior PR) provides the necessary boundaries:
`ota_writer`, `ota_header`, `ota_state`, and `ota_trigger_web` stubs are already in place.
This work fills in the stubs.

---

## Design Decisions

### URL Source

The OTA index URL is **hardcoded as a compile-time default** in the component. No NVS setup
is required for the device to work out of the box.

The web UI trigger endpoint accepts an optional URL override in the request body. If a URL
is provided it is used for that trigger; otherwise the hardcoded default is used. NVS
persistence for a saved override URL is a future enhancement.

### Re-interview After Wi-Fi OTA

After a successful Wi-Fi OTA the device restarts. Z2M re-learns the new version via the
existing Query Image Request timer: when the device rejoins and sends its next query, it
includes the new `current_file_version`. Z2M sees the device is already on the target
version and marks the update complete.

No special boot logic, no synthetic Upgrade End Request, no forced re-interview needed.
This is Option B — reliability over speed; Z2M updates within ~5 minutes of rejoin.

### Transport Selection (Z2M-triggered path)

When a Query Image Response arrives:
1. `ota_trigger_z2m_on_image_available()` calls `ota_wifi_transport_is_connected()`
2. If connected → acquire slot as `OTA_SOURCE_WIFI`, start Wi-Fi download using hardcoded URL
3. If not connected → start Zigbee block transfer as normal

Transport is committed at this point. There is no mid-OTA switch.

### Z2M Behaviour When Wi-Fi Is Selected

If Wi-Fi transport is chosen, the device never sends an Image Block Request. Z2M sent the
Query Image Response and waits; after its OTA timeout it marks the attempt failed on its
side. This is harmless — the device rejoins post-restart with the new version and Z2M
updates state via the re-query.

### Restart Mechanism

Wi-Fi transport uses an `esp_timer` one-shot (3 s delay) for the post-OTA restart.
Zigbee transport uses `esp_zb_scheduler_alarm` — these are different contexts and the
Zigbee scheduler is not available from a FreeRTOS task.

---

## Scope

### `esp32-zigbee-ota` component changes

| File | Change |
|------|--------|
| `src/ota_wifi_transport.c` | Implement HTTP download loop, 62-byte header strip, partition write, `esp_timer` restart |
| `src/ota_wifi_transport.h` | Add `OTA_WIFI_DEFAULT_URL` constant declaration |
| `src/ota_trigger_z2m.c` | Pass hardcoded URL to `ota_wifi_transport_start()` when Wi-Fi selected |
| `CMakeLists.txt` | Add `esp_wifi` back to C6 conditional REQUIRES (needed by real `is_connected` impl) |

**`ota_wifi_transport_run()` flow:**
```
ota_header_reset(OTA_HEADER_MODE_FULL_FILE)   ← strip 62 bytes (56-byte file header + 6-byte element header)
ota_writer_begin()
esp_http_client GET → per chunk:
    ota_header_process(chunk, ...)
    ota_writer_write(firmware_bytes)
ota_writer_finish()
ota_state_notify(SUCCESS, 100)
ota_state_release()
esp_timer one-shot 3s → esp_restart()

On failure:
ota_writer_abort()
ota_state_notify(FAILED, 0)
ota_state_release()
```

### `ld2450-zb-h2` firmware changes (C6 target)

| Item | Detail |
|------|--------|
| Hardcoded OTA URL | `OTA_WIFI_DEFAULT_URL` defined in `ota_wifi_transport.h`, value points to the OTA index |
| Web UI endpoint | HTTP POST `/ota` with optional `url` field in JSON body; calls `ota_trigger_web_start(url)` |
| Response codes | 202 Accepted (download started), 409 Conflict (OTA already in progress), 400 Bad Request (no URL and no default) |
| Web UI page | URL input box pre-populated with hardcoded default; submit triggers POST |

---

## What Is Explicitly Out of Scope

- NVS persistence for the URL override (future — "save as default" in web UI)
- Progress reporting during Wi-Fi download (0% → done is acceptable)
- Sending a Zigbee Upgrade End Request after Wi-Fi OTA (no SDK function; re-query handles it)
- H2 target (no Wi-Fi hardware; C6-only compilation enforced by CMakeLists.txt)

---

## Verification

1. C6 build includes all 7 source files, no `esp_wifi.h` include errors
2. H2 build excludes `ota_wifi_transport.c` and `ota_trigger_web.c`
3. Trigger Web UI OTA on C6 with Wi-Fi connected → firmware downloads and installs
4. Confirm Z2M updates device version within ~5 minutes of rejoin (re-query path)
5. Trigger Web UI OTA while Zigbee OTA in progress → HTTP 409
6. Trigger Web UI OTA with custom URL → uses provided URL, not default
7. Trigger Z2M OTA with Wi-Fi connected → Wi-Fi transport selected, Zigbee blocks not sent
8. Trigger Z2M OTA with Wi-Fi disconnected → falls back to Zigbee block transfer
