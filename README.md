# ESP32 Zigbee OTA Component

Reusable ESP-IDF component providing Zigbee Over-The-Air (OTA) firmware update capability for ESP32-H2/C6 projects.

## Features

- ✅ **Clean API**: Simple initialization and automatic operation
- ✅ **Standard ESP-IDF component**: Works with ESP-IDF v5.3+
- ✅ **Dual OTA partitions**: Automatic rollback on boot failure
- ✅ **Progress tracking**: Optional status callbacks for UI feedback
- ✅ **Z2M compatible**: Works with Zigbee2MQTT OTA server out of the box
- ✅ **Automatic queries**: Configurable interval-based update checks
- ✅ **Comprehensive logging**: Detailed progress and error reporting
- ✅ **Tag header handling**: Automatically strips OTA element tag header from firmware images

## Requirements

- **ESP-IDF**: v5.3.0 or newer
- **esp-zigbee-lib**: v1.0.0 or newer
- **Flash size**: 4MB minimum (for dual OTA partitions)
- **Target chips**: ESP32-H2, ESP32-C6

## Installation

### Method 1: ESP-IDF Component Manager (Recommended)

Add to your project's `main/idf_component.yml`:

```yaml
dependencies:
  zigbee_ota:
    git: https://github.com/ShaunPCcom/esp32-zigbee-ota.git
    version: ">=1.0.0"
```

The component will be automatically fetched when you run `idf.py build`.

### Method 2: Git Submodule

```bash
cd your_project/
git submodule add https://github.com/ShaunPCcom/esp32-zigbee-ota.git components/zigbee_ota
git submodule update --init
```

### Method 3: Manual Copy

Copy the entire repository into your project's `components/zigbee_ota/` directory.

## Partition Table Setup

### 1. Create Partition Table

Copy the reference partition table to your project:

```bash
cp components/zigbee_ota/partitions/partitions_ota_4mb.csv partitions.csv
```

Or create your own based on the reference. **Required partitions:**
- `nvs` - Configuration storage
- `otadata` - OTA state tracking
- `phy_init` - PHY calibration
- `ota_0` - First firmware partition
- `ota_1` - Second firmware partition
- `zb_storage` - Zigbee persistent data
- `zb_fct` - Zigbee factory data

### 2. Configure Project

Add to your project's `sdkconfig.defaults`:

```ini
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
```

## Integration

### 1. Include Header

```c
#include "zigbee_ota.h"
```

### 2. Initialize During Endpoint Creation

```c
/* Your existing endpoint setup */
esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();

/* Add your other clusters (Basic, Identify, etc.) */
esp_zb_cluster_list_add_basic_cluster(cluster_list, basic_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
// ... other clusters ...

/* Initialize OTA component */
zigbee_ota_config_t ota_config = ZIGBEE_OTA_CONFIG_DEFAULT();
ota_config.manufacturer_code = 0x131B;  // Espressif
ota_config.image_type = 0x0001;         // Your application ID
ota_config.current_file_version = 0x00010000;  // Version 1.0.0.0
ota_config.hw_version = 1;
ota_config.query_interval_minutes = 1440;  // Check every 24 hours

esp_err_t ret = zigbee_ota_init(cluster_list, 1, &ota_config);
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize OTA: %s", esp_err_to_name(ret));
}

/* Continue with endpoint registration */
esp_zb_ep_list_add_ep(ep_list, cluster_list, endpoint_config);
```

### 3. Register Action Handler

The OTA component needs to receive Zigbee action callbacks. Add this to your action handler:

```c
static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    /* Let OTA component handle OTA-related callbacks */
    esp_err_t ret = zigbee_ota_action_handler(callback_id, message);
    if (ret != ESP_ERR_NOT_SUPPORTED) {
        return ret;  /* OTA component handled it */
    }

    /* Handle your application callbacks */
    switch (callback_id) {
    case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID:
        // Your attribute handler
        break;
    // ... other callbacks ...
    default:
        ESP_LOGW(TAG, "Receive Zigbee action(0x%x) callback", callback_id);
        break;
    }
    return ESP_OK;
}

/* Register your action handler before esp_zb_start() */
esp_zb_core_action_handler_register(zb_action_handler);
```

### 4. Optional: Status Callback for UI Feedback

```c
static void ota_status_callback(zigbee_ota_status_t status, uint8_t progress_percent)
{
    switch (status) {
    case ZIGBEE_OTA_STATUS_START:
        ESP_LOGI(TAG, "OTA started");
        // Set LED to cyan/blue to indicate update in progress
        break;
    case ZIGBEE_OTA_STATUS_DOWNLOADING:
        ESP_LOGI(TAG, "OTA downloading: %d%%", progress_percent);
        // Update LED brightness or blink pattern based on progress
        break;
    case ZIGBEE_OTA_STATUS_DOWNLOAD_COMPLETE:
        ESP_LOGI(TAG, "OTA download complete, validating...");
        break;
    case ZIGBEE_OTA_STATUS_SUCCESS:
        ESP_LOGI(TAG, "OTA successful, restarting...");
        // Set LED to green
        break;
    case ZIGBEE_OTA_STATUS_FAILED:
        ESP_LOGW(TAG, "OTA failed");
        // Set LED to red, then resume normal operation
        break;
    default:
        break;
    }
}

/* Register callback after zigbee_ota_init() and before esp_zb_start() */
zigbee_ota_register_status_callback(ota_status_callback);
```

## Zigbee2MQTT Integration

### 1. Z2M Auto-Detection

Zigbee2MQTT automatically detects OTA capability when the OTA cluster (0x0019) is present. **No converter changes needed** - Z2M will show "Update available" in the UI when a new firmware is released.

### 2. Hosting Firmware Images

Host your firmware images on GitHub Releases:

1. Build your project: `idf.py build`
2. The OTA image is located at: `build/your_project.bin`
3. Create a GitHub release and upload the `.bin` file
4. Tag releases with version numbers matching your `current_file_version`

### 3. Creating OTA Images

Use the ESP Zigbee SDK image builder tool to create proper Zigbee OTA images:

```bash
# Install zigpy if needed
pip install zigpy

# Create OTA image from firmware binary
python $IDF_PATH/../esp-zigbee-sdk/tools/mfg_tool/image_builder_tool.py \
    -mn "Espressif" \
    -mc 0x131B \
    -mt 0x0001 \
    -fv 0x00010001 \
    -in build/your_project.bin \
    -o your_project_v1.0.0.1.ota
```

**Parameters:**
- `-mn`: Manufacturer name (displayed in Z2M)
- `-mc`: Manufacturer code (decimal, 0x131B = 4891 for Espressif)
- `-mt`: Image type (your application ID, decimal)
- `-fv`: File version (decimal, 0x00010001 = v1.0.0.1)
- `-in`: Input firmware binary (from `build/`)
- `-o`: Output OTA file name

### 4. OTA Index File (Optional)

For automatic update discovery, create an `index.json` file in your repo:

```json
[
  {
    "manufacturerCode": 4891,
    "imageType": 1,
    "fileVersion": 65537,
    "url": "ota/your_project_v1.0.0.1.ota"
  }
]
```

**Field mapping:**
- `manufacturerCode`: Decimal of your `manufacturer_code` (0x131B = 4891 for Espressif)
- `imageType`: Your `image_type` value
- `fileVersion`: Your `current_file_version` as decimal (0x00010000 = 65536 for v1.0.0.0)
- `fileSize`: Size of the `.bin` file in bytes
- `url`: Direct download URL to the firmware binary

**For Z2M integration:**

1. Place `index.json` in `/config/zigbee2mqtt/` directory
2. Place OTA image files in `/config/zigbee2mqtt/ota/` subdirectory
3. Configure Z2M in `configuration.yaml`:

```yaml
ota:
  update_check_interval: 1440  # Check every 24 hours
  disable_automatic_update_check: false
  zigbee_ota_override_index_location: ota_index.json
```

**Note**: Use relative paths in `index.json` (`ota/filename.ota`) instead of absolute paths. Z2M resolves paths relative to the Zigbee2MQTT data directory (`/config/zigbee2mqtt/`).

## API Reference

### Configuration

```c
typedef struct {
    uint16_t manufacturer_code;         // Manufacturer code (0x131B for Espressif)
    uint16_t image_type;                // Application-specific image type
    uint32_t current_file_version;      // Current firmware version
    uint16_t hw_version;                // Hardware version
    uint16_t query_interval_minutes;    // Auto-query interval (0=disabled, default=1440)
    uint8_t max_data_size;              // Max OTA block size (default=64)
} zigbee_ota_config_t;
```

### Functions

| Function | Description |
|----------|-------------|
| `zigbee_ota_init()` | Initialize OTA client, add cluster to endpoint |
| `zigbee_ota_register_status_callback()` | Register callback for status updates (optional) |
| `zigbee_ota_action_handler()` | Call from your action handler to route OTA callbacks |
| `zigbee_ota_start_query()` | Manually trigger OTA query (optional) |
| `zigbee_ota_set_query_interval()` | Change auto-query interval (optional) |

## Version Numbering

The `current_file_version` is a 32-bit value. Recommended format:

```
0xMMmmppbb
  MM = Major version
  mm = Minor version
  pp = Patch version
  bb = Build number

Example: v1.2.3.4 = 0x01020304
```

Update this value in your code when releasing new firmware versions.

## Technical Details

### OTA Image Format Handling

Zigbee OTA images have a specific structure:
```
[56-byte OTA header] [6-byte Tag header] [ESP32 firmware binary]
```

The 6-byte tag header consists of:
- 2 bytes: Tag ID (0x0000 = upgrade image element)
- 4 bytes: Tag length (payload size)

**ESP32 firmware validation**: The ESP-IDF OTA system (`esp_ota_write()`) expects raw ESP32 firmware starting with the magic byte `0xE9`. If the tag header is not stripped, validation fails with:

```
E (xxxxx) esp_ota_ops: OTA image has invalid magic byte (expected 0xE9, saw 0x00)
```

**Automatic handling**: This component automatically detects and strips the 6-byte tag header from the first data chunk before writing to the OTA partition. No special configuration needed.

**Reference**: ESP Zigbee SDK OTA client example uses `esp_element_ota_data()` function for the same purpose. This component implements the same logic inline for simplicity.

## Troubleshooting

### OTA partition not found
**Error:** `No OTA partition found`
**Solution:** Ensure your partition table includes `ota_0` and `ota_1` partitions. Use `idf.py partition-table` to verify.

### Invalid magic byte error
**Error:** `OTA image has invalid magic byte (expected 0xE9, saw 0x00)`
**Solution:** This should be handled automatically by the component (tag header stripping). If you see this error, ensure you're using the latest version of this component (v1.0.1+). The fix was added in commit 095da09.

### Z2M doesn't show update available
**Check:**
1. Device has OTA cluster (0x0019) - visible in Z2M device clusters
2. Firmware version in GitHub release is **higher** than `current_file_version`
3. `manufacturerCode` and `imageType` match between device and OTA index
4. Z2M OTA index is configured correctly

### Update fails during download
**Common causes:**
- Network interruption (Zigbee mesh instability)
- Insufficient flash space (check partition sizes)
- Corrupted firmware image

The component automatically aborts on errors. Device will continue running current firmware.

### Device reboots but doesn't update
**Solution:** This indicates the bootloader rolled back to the previous partition. Check:
1. New firmware is valid (builds without errors)
2. Partition table matches between old and new firmware
3. Serial logs for bootloader error messages

## Example Projects

- **LD2450 Zigbee Sensor**: [ShaunPCcom/ESP32-H2-LD2450](https://github.com/ShaunPCcom/ESP32-H2-LD2450)
- **Zigbee LED Controller**: [ShaunPCcom/zb-h2-LED-lighting](https://github.com/ShaunPCcom/zb-h2-LED-lighting) *(integration pending)*

## License

MIT License - see [LICENSE](LICENSE) file for details.

## Contributing

Issues and pull requests welcome! Please open an issue before starting work on major changes.

## Acknowledgments

Based on ESP-IDF and esp-zigbee-sdk OTA examples from Espressif Systems.
