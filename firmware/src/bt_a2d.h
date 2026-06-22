#pragma once

#include <stdbool.h>

#include "esp_bt_defs.h"
#include "esp_err.h"

// BT classic + A2DP source, thin wrapper over the IDF Bluedroid APIs.
// Init brings the controller and source endpoint up but does NOT initiate
// any connection. The app_sm state machine drives connection attempts,
// pairing entry, and audio start/stop via the calls below.
//
// Events fire on the bt_a2d worker task; the callback must not block.

typedef enum {
    BT_EV_CONNECTED,         // A2DP link established (peer BDA in `bda`)
    BT_EV_DISCONNECTED,      // A2DP link torn down
    BT_EV_AUDIO_STARTED,     // sink reported audio_state STARTED
    BT_EV_AUDIO_SUSPENDED,   // sink reported audio_state SUSPEND
    BT_EV_PAIRING_TIMEOUT,   // inquiry finished with no matching sink
} bt_event_t;

typedef struct {
    bt_event_t type;
    esp_bd_addr_t bda;
} bt_event_msg_t;

typedef void (*bt_event_cb_t)(const bt_event_msg_t *msg);

// Subscribe to BT-layer events. Only one subscriber; second call replaces.
// May be called before or after bt_a2d_init().
void bt_a2d_set_event_cb(bt_event_cb_t cb);

// Bring up controller + Bluedroid + GAP + A2DP source endpoint. Idempotent
// guard via internal flag.
esp_err_t bt_a2d_init(void);

// Page a bonded peer. Tries the most-recently-connected sink first, then
// the rest of the bond list in order, advancing one slot per call so
// successive reconnect ticks cycle through every bonded sink. Returns
// ESP_OK if a page attempt was issued (BT_EV_CONNECTED or BT_EV_DISCONNECTED
// follows); ESP_ERR_NOT_FOUND if no bonded device exists.
esp_err_t bt_a2d_connect_bonded(void);

// Enter pairing: start a general inquiry, match the first A2DP sink to
// answer (rendering service in its class-of-device, no name filter), and
// connect to it. The successful pair is persisted to NVS by Bluedroid
// automatically. Emits BT_EV_PAIRING_TIMEOUT if the inquiry completes
// without a match.
esp_err_t bt_a2d_start_pairing(void);

// True if at least one sink is currently bonded. Lets the state machine
// decide, when pairing gives up, whether to sleep (no bonds) or fall back
// to bonded reconnect (bonds exist).
bool bt_a2d_has_bond(void);

// Factory reset: remove every bonded device (clears all Bluedroid link
// keys) and drop the most-recently-connected NVS record. After this, the
// next bt_a2d_connect_bonded returns ESP_ERR_NOT_FOUND so the state machine
// falls into pairing.
esp_err_t bt_a2d_clear_bonds(void);

// Cancel any in-progress discovery and disconnect an active A2DP link.
// No-op if already idle.
esp_err_t bt_a2d_disconnect(void);

// Drive the A2DP media stream. STARTED gates the actual SBC encoding of
// the PCM stream-buffer contents toward the sink; SUSPEND tells the sink
// to stop expecting audio. Only meaningful while connected.
esp_err_t bt_a2d_media_start(void);
esp_err_t bt_a2d_media_suspend(void);
