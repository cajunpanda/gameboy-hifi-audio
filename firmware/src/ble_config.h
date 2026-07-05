#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// BLE GATT config server beside the classic A2DP source: one custom 128-bit
// service with a packed-settings read/write/notify characteristic plus an action
// (save/chime/play) write characteristic. A Web-Bluetooth page connects directly
// and reads/writes the DSP settings over the same settings_*/sfx_* API the UART
// console uses.
//
// Requires the controller in BTDM dual mode and Bluedroid up: call after
// bt_a2d_init(). Compiles to nothing unless CONFIG_GBHIFI_BLE_CONFIG is set
// (which depends on CONFIG_BT_BLE_ENABLED).
esp_err_t ble_config_init(void);

// ---- OTA (over-the-air firmware update) ------------------------------------
// The service also exposes OTACTL/OTADATA characteristics so the web page can
// push a new image over BLE. Audio streams live, so the flash erase/writes must
// not race the audio path: the state machine quiesces first. ble_config fires
// BLE_OTA_EV_BEGIN_REQUEST when the client sends BEGIN (it does not touch flash
// yet), app_sm stops the pipeline and drops A2DP, then calls
// ble_config_ota_proceed() to start the transfer. BLE_OTA_EV_FINISHED fires on
// any non-reboot end (abort / error / client disconnect / stall) so app_sm can
// resume normal operation. A clean success reboots the chip (no FINISHED).

typedef enum {
    BLE_OTA_EV_BEGIN_REQUEST,  // client asked to start; app_sm should quiesce then call ble_config_ota_proceed()
    BLE_OTA_EV_FINISHED,       // OTA ended without reboot; app_sm should resume normal operation
} ble_ota_event_t;

typedef void (*ble_ota_event_cb_t)(ble_ota_event_t ev);

// Subscribe to OTA lifecycle events. One subscriber (app_sm); second call
// replaces. Safe to call before ble_config_init().
void ble_config_set_ota_cb(ble_ota_event_cb_t cb);

// Called by app_sm once the audio path is quiesced (pipeline stopped, A2DP
// dropped) in response to BLE_OTA_EV_BEGIN_REQUEST: selects the inactive OTA
// slot, esp_ota_begin()s it (the multi-hundred-ms erase happens here), and
// notifies the client READY so it starts streaming chunks. No-op (ESP_OK) if no
// begin is pending (e.g. the request already timed out). Returns the
// esp_ota_begin() result.
esp_err_t ble_config_ota_proceed(void);

// Start/stop BLE advertising. The `radio off` console command uses this to silence
// the periodic advertising RF bursts for a clean noise measurement; `off` also
// inhibits the auto re-advertise on central disconnect until turned back on.
void ble_config_set_advertising(bool on);

#ifdef __cplusplus
}
#endif
