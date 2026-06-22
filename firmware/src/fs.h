#pragma once

#include "esp_err.h"

// Mount point for the audio clip store (the `storage` LittleFS partition).
// Clips live at FS_CLIPS_MOUNT "/<name>.gsfx" and are read by the cue feeder in
// sfx.c. Mounted read-write so the web UI can upload/replace clips at runtime.
#define FS_CLIPS_MOUNT "/clips"

// Mount the LittleFS clip-store partition. Formats it if the mount fails
// (first boot after a fresh flash with no FS image, or a corrupt FS). Call
// once at boot before sfx_init().
esp_err_t fs_init(void);

