FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.c)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "."
    # app_update: esp_ota_ops.h for the BLE OTA path.
    PRIV_REQUIRES bt nvs_flash driver esp_timer esp-dsp littlefs console vfs app_update
)

# Bake firmware/data/ into the `storage` LittleFS partition image and flash it
# alongside the app (FLASH_IN_PROJECT). Holds the bundled audio clips; the web
# UI can overwrite files at runtime over the same filesystem.
littlefs_create_partition_image(storage ${CMAKE_SOURCE_DIR}/data FLASH_IN_PROJECT)
