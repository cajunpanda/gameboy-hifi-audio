#include "fs.h"

#include "esp_littlefs.h"
#include "esp_log.h"

static const char *TAG = "fs";

#define STORAGE_PARTITION_LABEL "storage"

esp_err_t fs_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path              = FS_CLIPS_MOUNT,
        .partition_label        = STORAGE_PARTITION_LABEL,
        .format_if_mount_failed = true,
        .dont_mount             = false,
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "littlefs mount failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t total = 0, used = 0;
    if (esp_littlefs_info(STORAGE_PARTITION_LABEL, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "clips mounted at %s: %u/%u bytes used",
                 FS_CLIPS_MOUNT, (unsigned)used, (unsigned)total);
    } else {
        ESP_LOGI(TAG, "clips mounted at %s", FS_CLIPS_MOUNT);
    }
    return ESP_OK;
}

