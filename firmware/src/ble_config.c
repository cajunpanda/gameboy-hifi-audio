#include "ble_config.h"

#include "sdkconfig.h"

#if CONFIG_GBHIFI_BLE_CONFIG

#include <stdio.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_common_api.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"

#include "esp_ota_ops.h"     // OTA: esp_ota_begin/write/end, partition select
#include "esp_app_desc.h"    // OTA: running app version for GET_INFO
#include "esp_rom_crc.h"     // OTA: incremental crc32 sanity check

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"

#include "esp_console.h"     // BLE console: esp_console_run() reuses the UART cmd table

#include <stdarg.h>
#include <stdlib.h>

#include "settings.h"
#include "sfx.h"
#include "audio_pipeline.h"   // spectrum tap for the BLE web visualizer
#include "app_sm.h"           // app_sm_hp_plugged(): distinguishes speaker vs headphone for the spectrum source byte

static const char *TAG = "ble_cfg";

#define BLE_DEVICE_NAME   "GameBoy HiFi Setup"
#define BLE_APP_ID        0x42
#define ACTION_MAX_LEN    40         // opcode + clip-name room

// ---- GATT schema ----------------------------------------------------------
// Custom 128-bit UUIDs, stored LSB-first as Bluedroid expects.
//   service:  5a4d0000-1f2e-4c6b-9a8f-0102030405aa
//   settings: 5a4d0001-...   (R/W/Notify, packed gbhifi_settings_t mirror)
//   action:   5a4d0002-...   (Write: save / chime / play)
static const uint8_t SVC_UUID128[16] = {
    0xaa, 0x05, 0x04, 0x03, 0x02, 0x01, 0x8f, 0x9a,
    0x6b, 0x4c, 0x2e, 0x1f, 0x00, 0x00, 0x4d, 0x5a,
};
static const uint8_t SETTINGS_UUID128[16] = {
    0xaa, 0x05, 0x04, 0x03, 0x02, 0x01, 0x8f, 0x9a,
    0x6b, 0x4c, 0x2e, 0x1f, 0x01, 0x00, 0x4d, 0x5a,
};
static const uint8_t ACTION_UUID128[16] = {
    0xaa, 0x05, 0x04, 0x03, 0x02, 0x01, 0x8f, 0x9a,
    0x6b, 0x4c, 0x2e, 0x1f, 0x02, 0x00, 0x4d, 0x5a,
};
//   otactl:  5a4d0003-...   (Write + Notify: OTA control / status)
//   otadata: 5a4d0004-...   (Write-no-response: raw firmware chunks)
static const uint8_t OTACTL_UUID128[16] = {
    0xaa, 0x05, 0x04, 0x03, 0x02, 0x01, 0x8f, 0x9a,
    0x6b, 0x4c, 0x2e, 0x1f, 0x03, 0x00, 0x4d, 0x5a,
};
static const uint8_t OTADATA_UUID128[16] = {
    0xaa, 0x05, 0x04, 0x03, 0x02, 0x01, 0x8f, 0x9a,
    0x6b, 0x4c, 0x2e, 0x1f, 0x04, 0x00, 0x4d, 0x5a,
};
//   log:     5a4d0005-...   (Notify: device -> client ESP_LOG / console stream)
//   cmd:     5a4d0006-...   (Write / Write-NR: client -> device console line)
static const uint8_t LOG_UUID128[16] = {
    0xaa, 0x05, 0x04, 0x03, 0x02, 0x01, 0x8f, 0x9a,
    0x6b, 0x4c, 0x2e, 0x1f, 0x05, 0x00, 0x4d, 0x5a,
};
static const uint8_t CMD_UUID128[16] = {
    0xaa, 0x05, 0x04, 0x03, 0x02, 0x01, 0x8f, 0x9a,
    0x6b, 0x4c, 0x2e, 0x1f, 0x06, 0x00, 0x4d, 0x5a,
};
//   spec:    5a4d0007-...   (Notify: stereo live audio spectrum, L bands + R bands)
static const uint8_t SPEC_UUID128[16] = {
    0xaa, 0x05, 0x04, 0x03, 0x02, 0x01, 0x8f, 0x9a,
    0x6b, 0x4c, 0x2e, 0x1f, 0x07, 0x00, 0x4d, 0x5a,
};

// Settings wire format: explicit little-endian byte layout, decoupled from the
// gbhifi_settings_t memory layout so compiler padding never reaches the air and
// the JS side can DataView-parse a stable schema. Keep in lockstep with the web
// UI's encoder/decoder. 28 bytes needs an ATT MTU >= 31, so ble_config_init()
// raises the local MTU (Web Bluetooth negotiates well above that).
#define SETTINGS_WIRE_LEN 28
//   [0..1] version u16   [2] spk_vol   [3] bt_vol
//   [4] eq_en [5] eq_bass [6] eq_mid [7] eq_treble (i8)  Speaker EQ
//   [8] eq_bt_en [9] eq_bt_bass [10] eq_bt_mid [11] eq_bt_treble (i8)  Bluetooth EQ
//   [12] sfx_en [13] sfx_level (i8)
//   [14] mode_a (1=bypass; READ-ONLY status, writes ignored) [15] boot_mode_a
//   [16..17] hold_connect_ms u16  [18..19] hold_pair_ms u16
//   [20..21] hold_mode_ms u16     [22..23] hold_mode_exit_ms u16
//   [24] eq_hp_en [25] eq_hp_bass [26] eq_hp_mid [27] eq_hp_treble (i8)  Headphone EQ

// Action characteristic opcodes (byte 0 of the write).
enum {
    ACT_SAVE  = 0x01,  // no args            -> settings_commit()
    ACT_CHIME = 0x02,  // arg: synth id      -> sfx_trigger_synth()
    ACT_PLAY  = 0x03,  // arg: clip name str -> sfx_trigger_clip()
};

// ---- OTA protocol (keep in lockstep with web/index.html) ------------------
// OTACTL opcodes (client -> device, byte 0 of the write):
enum {
    OTA_OP_BEGIN    = 0x01,  // [u32 size][u32 crc32] -> request transfer (device quiesces, then READY)
    OTA_OP_END      = 0x02,  // no args               -> finalize + reboot
    OTA_OP_ABORT    = 0x03,  // no args               -> abort + clean up
    OTA_OP_GET_INFO = 0x04,  // no args               -> reply INFO (running slot + version)
};
// OTACTL status codes (device -> client, byte 0 of the notify):
enum {
    OTA_ST_READY = 0x10,  // [u32 chunk_max][u32 window] -> begin accepted, stream now
    OTA_ST_ACK   = 0x11,  // [u32 received]              -> flow-control ack
    OTA_ST_DONE  = 0x12,  // no args                     -> verified, rebooting
    OTA_ST_INFO  = 0x13,  // ascii "version|slot|builddate" -> reply to GET_INFO
    OTA_ST_ERROR = 0x1f,  // [u8 code][ascii msg]        -> failure
};
#define OTA_WINDOW     8192   // bytes the client may send unacked before waiting
#define OTA_CHUNK_MAX  500    // per-write payload cap (also bounded by the MTU)
#define OTA_STALL_MS   15000  // abort a transfer (or an un-proceeded begin) idle this long

// ---- BLE debug console (log stream + command input) -----------------------
// LOG (notify): mirrors the ESP_LOG output the UART console carries, so a BLE
// client can `tail -f` the device without the Tag-Connect cable. CMD (write):
// one console command line, dispatched through the same esp_console command
// table the UART REPL registers. Both are purely additive; the UART logger and
// REPL are untouched (the LOG vprintf hook chains to the previous logger).
#define BLE_LOG_VAL_MAX  512    // max LOG notify payload (also the value attr length)
#define BLE_CMD_VAL_MAX  160    // max console line accepted on CMD
#define BLE_LOG_SB_SIZE  2048   // LOG stream-buffer capacity; overflow silently drops

// A2DP streaming and the BLE stack share the internal (DMA-capable) heap. Under
// heavy load -- a BT sink streaming while the web config is open -- the SBC
// encoder can be starved of its ~4 KB TX buffers. Never send a BLE notification
// (log or spectrum) when free internal heap is below this floor: audio wins, and
// this breaks the "BT error -> log tee -> more BLE allocs -> worse" spiral.
#define BLE_NOTIFY_HEAP_FLOOR  18000

// ---- audio spectrum (web visualizer) --------------------------------------
// SPECTRUM (notify): a stereo bar spectrum of the live audio the user is hearing,
// streamed at ~20 fps while a client is subscribed. Payload byte 0 is the source
// (0 = speaker, 1 = Bluetooth, 2 = headphone), then SPEC_BINS left bytes and SPEC_BINS
// right bytes (0..255 each). Gated on the CCCD: the audio-side FFT tap only runs
// while someone is watching.
#define SPEC_BINS         32    // bands per channel (payload = 1 + 2 * SPEC_BINS)
#define SPEC_INTERVAL_MS  50    // ~20 fps

// Attribute-table indices.
enum {
    IDX_SVC,
    IDX_SETTINGS_DECL,
    IDX_SETTINGS_VAL,
    IDX_SETTINGS_CCCD,
    IDX_ACTION_DECL,
    IDX_ACTION_VAL,
    IDX_OTACTL_DECL,
    IDX_OTACTL_VAL,
    IDX_OTACTL_CCCD,
    IDX_OTADATA_DECL,
    IDX_OTADATA_VAL,
    IDX_LOG_DECL,
    IDX_LOG_VAL,
    IDX_LOG_CCCD,
    IDX_CMD_DECL,
    IDX_CMD_VAL,
    IDX_SPEC_DECL,
    IDX_SPEC_VAL,
    IDX_SPEC_CCCD,
    IDX_NB,
};

// 16-bit declaration UUIDs + property bytes for the attribute table.
static const uint16_t k_pri_service_uuid = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t k_char_decl_uuid   = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t k_cccd_uuid        = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
static const uint8_t  k_prop_rwn = ESP_GATT_CHAR_PROP_BIT_READ |
                                   ESP_GATT_CHAR_PROP_BIT_WRITE |
                                   ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static const uint8_t  k_prop_w   = ESP_GATT_CHAR_PROP_BIT_WRITE;
static const uint8_t  k_prop_wn  = ESP_GATT_CHAR_PROP_BIT_WRITE |
                                   ESP_GATT_CHAR_PROP_BIT_NOTIFY;     // OTACTL
static const uint8_t  k_prop_wnr = ESP_GATT_CHAR_PROP_BIT_WRITE_NR;  // OTADATA
static const uint8_t  k_prop_n   = ESP_GATT_CHAR_PROP_BIT_NOTIFY;    // LOG
static const uint8_t  k_prop_cmd = ESP_GATT_CHAR_PROP_BIT_WRITE |
                                   ESP_GATT_CHAR_PROP_BIT_WRITE_NR;  // CMD
static uint8_t        s_cccd_val[2]     = {0, 0};  // settings CCCD (stack-managed)
static uint8_t        s_cccd_ota_val[2] = {0, 0};  // OTACTL CCCD (stack-managed)
static uint8_t        s_cccd_log_val[2]  = {0, 0};  // LOG CCCD (stack-managed)
static uint8_t        s_cccd_spec_val[2] = {0, 0};  // SPECTRUM CCCD (stack-managed)
static uint8_t        s_ota_attr_dummy  = 0;       // placeholder initial value for the AUTO_RSP OTA chars

static const esp_gatts_attr_db_t k_gatt_db[IDX_NB] = {
    // Primary service.
    [IDX_SVC] = { {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&k_pri_service_uuid, ESP_GATT_PERM_READ,
         sizeof(SVC_UUID128), sizeof(SVC_UUID128), (uint8_t *)SVC_UUID128} },

    // Settings characteristic: declaration + value (served by the app) + CCCD.
    [IDX_SETTINGS_DECL] = { {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&k_char_decl_uuid, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(uint8_t), (uint8_t *)&k_prop_rwn} },
    [IDX_SETTINGS_VAL] = { {ESP_GATT_RSP_BY_APP},
        {ESP_UUID_LEN_128, (uint8_t *)SETTINGS_UUID128,
         ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
         SETTINGS_WIRE_LEN, 0, NULL} },
    [IDX_SETTINGS_CCCD] = { {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&k_cccd_uuid,
         ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
         sizeof(s_cccd_val), sizeof(s_cccd_val), s_cccd_val} },

    // Action characteristic: declaration + value (write-only, served by app).
    [IDX_ACTION_DECL] = { {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&k_char_decl_uuid, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(uint8_t), (uint8_t *)&k_prop_w} },
    [IDX_ACTION_VAL] = { {ESP_GATT_RSP_BY_APP},
        {ESP_UUID_LEN_128, (uint8_t *)ACTION_UUID128, ESP_GATT_PERM_WRITE,
         ACTION_MAX_LEN, 0, NULL} },

    // OTACTL characteristic: declaration + value (write + notify) + CCCD.
    // AUTO_RSP: the stack buffers the (small) control writes and acks them; we
    // still get ESP_GATTS_WRITE_EVT to parse the opcode, and we notify status
    // back on the same value handle.
    [IDX_OTACTL_DECL] = { {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&k_char_decl_uuid, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(uint8_t), (uint8_t *)&k_prop_wn} },
    [IDX_OTACTL_VAL] = { {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_128, (uint8_t *)OTACTL_UUID128, ESP_GATT_PERM_WRITE,
         64, sizeof(s_ota_attr_dummy), &s_ota_attr_dummy} },
    [IDX_OTACTL_CCCD] = { {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&k_cccd_uuid,
         ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
         sizeof(s_cccd_ota_val), sizeof(s_cccd_ota_val), s_cccd_ota_val} },

    // OTADATA characteristic: declaration + value (write-without-response). The
    // raw firmware chunks land here; AUTO_RSP buffers each write (<= MTU-3) and
    // fires ESP_GATTS_WRITE_EVT. No CCCD (never notified).
    [IDX_OTADATA_DECL] = { {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&k_char_decl_uuid, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(uint8_t), (uint8_t *)&k_prop_wnr} },
    [IDX_OTADATA_VAL] = { {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_128, (uint8_t *)OTADATA_UUID128, ESP_GATT_PERM_WRITE,
         512, sizeof(s_ota_attr_dummy), &s_ota_attr_dummy} },

    // LOG characteristic: notify-only ESP_LOG/console stream + CCCD. AUTO_RSP;
    // the value is never read/written by the client, only notified from
    // ble_log_task. Client subscribes via the CCCD to start the stream.
    [IDX_LOG_DECL] = { {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&k_char_decl_uuid, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(uint8_t), (uint8_t *)&k_prop_n} },
    [IDX_LOG_VAL] = { {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_128, (uint8_t *)LOG_UUID128, ESP_GATT_PERM_READ,
         BLE_LOG_VAL_MAX, sizeof(s_ota_attr_dummy), &s_ota_attr_dummy} },
    [IDX_LOG_CCCD] = { {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&k_cccd_uuid,
         ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
         sizeof(s_cccd_log_val), sizeof(s_cccd_log_val), s_cccd_log_val} },

    // CMD characteristic: declaration + value. The client writes one console
    // command line here (AUTO_RSP buffers the write, fires WRITE_EVT; we copy +
    // queue it for ble_cmd_task). No CCCD (never notified).
    [IDX_CMD_DECL] = { {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&k_char_decl_uuid, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(uint8_t), (uint8_t *)&k_prop_cmd} },
    [IDX_CMD_VAL] = { {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_128, (uint8_t *)CMD_UUID128, ESP_GATT_PERM_WRITE,
         BLE_CMD_VAL_MAX, sizeof(s_ota_attr_dummy), &s_ota_attr_dummy} },

    // SPECTRUM characteristic: notify-only stereo bar spectrum + CCCD. Notified
    // from ble_spec_task at ~20 fps while subscribed; never read/written.
    [IDX_SPEC_DECL] = { {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&k_char_decl_uuid, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(uint8_t), (uint8_t *)&k_prop_n} },
    [IDX_SPEC_VAL] = { {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_128, (uint8_t *)SPEC_UUID128, ESP_GATT_PERM_READ,
         1 + 2 * SPEC_BINS, sizeof(s_ota_attr_dummy), &s_ota_attr_dummy} },
    [IDX_SPEC_CCCD] = { {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&k_cccd_uuid,
         ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
         sizeof(s_cccd_spec_val), sizeof(s_cccd_spec_val), s_cccd_spec_val} },
};

// ---- runtime state --------------------------------------------------------
static esp_gatt_if_t s_gatts_if      = ESP_GATT_IF_NONE;
static uint16_t      s_handles[IDX_NB] = {0};
static uint16_t      s_conn_id       = 0;
static esp_bd_addr_t s_peer_bda      = {0};  // current central, for conn-param updates
static bool          s_connected     = false;
static bool          s_notify_on     = false;
static uint32_t      s_last_gen      = 0;
static esp_timer_handle_t s_notify_timer = NULL;
static uint16_t      s_mtu           = 23;   // negotiated ATT MTU (ESP_GATTS_MTU_EVT)

// ---- BLE debug console state ----------------------------------------------
static bool                 s_log_notify_on = false;  // client subscribed to LOG CCCD
static StreamBufferHandle_t s_log_sb        = NULL;    // ESP_LOG bytes -> ble_log_task
static QueueHandle_t        s_cmd_q         = NULL;    // char* command lines -> ble_cmd_task
static TaskHandle_t         s_log_task      = NULL;    // drains s_log_sb (feedback guard)
static vprintf_like_t       s_prev_vprintf  = NULL;    // UART logger we chain to

// ---- audio spectrum state -------------------------------------------------
static bool          s_spec_notify_on = false;   // client subscribed to SPECTRUM CCCD
static TaskHandle_t  s_spec_task      = NULL;     // computes + notifies the spectrum

// ---- OTA transfer state ---------------------------------------------------
static ble_ota_event_cb_t s_ota_cb       = NULL;
static esp_ota_handle_t    s_ota_handle  = 0;
static const esp_partition_t *s_ota_part = NULL;
static bool     s_ota_pending  = false;  // BEGIN received, waiting for app_sm to proceed
static bool     s_ota_active   = false;  // esp_ota_begin() done, streaming chunks
static uint32_t s_ota_size     = 0;
static uint32_t s_ota_recv     = 0;
static uint32_t s_ota_acked    = 0;
static uint32_t s_ota_crc      = 0;      // running crc32 of received data
static uint32_t s_ota_exp_crc  = 0;      // crc32 the client promised
static int64_t  s_ota_touch_ms = 0;      // last BEGIN/data activity (stall watchdog)

// ---- settings <-> wire ----------------------------------------------------
static void settings_to_wire(uint8_t *b)
{
    gbhifi_settings_t s;
    settings_get(&s);
    b[0]  = (uint8_t)(s.version & 0xff);
    b[1]  = (uint8_t)(s.version >> 8);
    b[2]  = s.speaker_vol_pct;
    b[3]  = s.bt_vol_pct;
    b[4]  = s.eq_enabled ? 1 : 0;
    b[5]  = (uint8_t)s.eq_bass_db;
    b[6]  = (uint8_t)s.eq_mid_db;
    b[7]  = (uint8_t)s.eq_treble_db;
    b[8]  = s.eq_bt_enabled ? 1 : 0;
    b[9]  = (uint8_t)s.eq_bt_bass_db;
    b[10] = (uint8_t)s.eq_bt_mid_db;
    b[11] = (uint8_t)s.eq_bt_treble_db;
    b[12] = s.sfx_enabled ? 1 : 0;
    b[13] = (uint8_t)s.sfx_level_db;
    b[14] = s.mode_a ? 1 : 0;
    b[15] = s.boot_mode_a ? 1 : 0;
    b[16] = (uint8_t)(s.hold_connect_ms   & 0xff);
    b[17] = (uint8_t)(s.hold_connect_ms   >> 8);
    b[18] = (uint8_t)(s.hold_pair_ms      & 0xff);
    b[19] = (uint8_t)(s.hold_pair_ms      >> 8);
    b[20] = (uint8_t)(s.hold_mode_ms      & 0xff);
    b[21] = (uint8_t)(s.hold_mode_ms      >> 8);
    b[22] = (uint8_t)(s.hold_mode_exit_ms & 0xff);
    b[23] = (uint8_t)(s.hold_mode_exit_ms >> 8);
    b[24] = s.eq_hp_enabled ? 1 : 0;
    b[25] = (uint8_t)s.eq_hp_bass_db;
    b[26] = (uint8_t)s.eq_hp_mid_db;
    b[27] = (uint8_t)s.eq_hp_treble_db;
}

// Apply a settings write through the validating setters (live, not persisted;
// the host saves explicitly via the action char, same as the console's `save`).
// The version byte is informational on read and ignored on write.
//
// mode_a (b[14]) is deliberately not written here: entering Mode A makes the
// radio intermittent (duty-cycle light sleep), so a Web-UI "switch to Mode A"
// would be a one-way trip the UI can't reverse, stranding the user. The button
// gesture is the only user-facing mode control; the UI shows mode_a read-only
// as status.
static void wire_to_settings(const uint8_t *b)
{
    settings_set_volume(b[2]);
    settings_set_bt_volume(b[3]);
    settings_set_eq(b[4] != 0, (int8_t)b[5], (int8_t)b[6], (int8_t)b[7]);
    settings_set_hp_eq(b[24] != 0, (int8_t)b[25], (int8_t)b[26], (int8_t)b[27]);
    settings_set_bt_eq(b[8] != 0, (int8_t)b[9], (int8_t)b[10], (int8_t)b[11]);
    settings_set_sfx(b[12] != 0, (int8_t)b[13]);
    settings_set_boot_mode_a(b[15] != 0);
    settings_set_hold_timings((uint16_t)(b[16] | (b[17] << 8)),
                              (uint16_t)(b[18] | (b[19] << 8)),
                              (uint16_t)(b[20] | (b[21] << 8)),
                              (uint16_t)(b[22] | (b[23] << 8)));
}

static void do_action(const uint8_t *v, uint16_t len)
{
    if (len < 1) return;
    switch (v[0]) {
    case ACT_SAVE:
        ESP_LOGI(TAG, "action: save");
        settings_commit();
        break;
    case ACT_CHIME: {
        synth_id_t id = (len >= 2 && v[1] < SFX_SYNTH_COUNT)
                            ? (synth_id_t)v[1] : SFX_SYNTH_CONNECT;
        ESP_LOGI(TAG, "action: chime %d", id);
        sfx_trigger_synth(id);
        break;
    }
    case ACT_PLAY: {
        char name[ACTION_MAX_LEN] = {0};
        uint16_t n = len - 1;
        if (n >= sizeof(name)) n = sizeof(name) - 1;
        memcpy(name, v + 1, n);
        ESP_LOGI(TAG, "action: play \"%s\"", name);
        sfx_trigger_clip(name);
        break;
    }
    default:
        ESP_LOGW(TAG, "action: unknown opcode 0x%02x", v[0]);
        break;
    }
}

// ---- OTA (over-the-air firmware update) -----------------------------------
// The client streams the image as raw OTADATA chunks with windowed flow
// control; control and status ride OTACTL. BEGIN does not erase/flash
// immediately: audio streams live, so app_sm must stop the pipeline and drop
// A2DP first. BEGIN stashes size/crc and fires the cb; ble_config_ota_proceed()
// (called back by app_sm once quiesced) does the actual esp_ota_begin() + READY.

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

// Notify a status frame on OTACTL. Dropped silently if no central is connected.
static void ota_notify(const uint8_t *buf, uint16_t len)
{
    if (!s_connected || s_gatts_if == ESP_GATT_IF_NONE) return;
    esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id, s_handles[IDX_OTACTL_VAL],
                                len, (uint8_t *)buf, false /* notify, no confirm */);
}

static void ota_error(uint8_t code, const char *msg)
{
    uint8_t b[40];
    b[0] = OTA_ST_ERROR; b[1] = code;
    uint16_t n = 2;
    if (msg) {
        uint16_t m = (uint16_t)strlen(msg);
        if (m > sizeof(b) - 2) m = sizeof(b) - 2;
        memcpy(b + 2, msg, m); n = (uint16_t)(n + m);
    }
    ota_notify(b, n);
    ESP_LOGE(TAG, "OTA error %u: %s", code, msg ? msg : "");
}

// Tear down any in-flight transfer. fire_cb=true notifies app_sm (FINISHED) so
// it resumes the audio path. Used on abort/error/disconnect/stall, not on a
// clean success (that reboots, so resuming is moot).
static void ota_cleanup(bool fire_cb)
{
    if (s_ota_active && s_ota_handle) esp_ota_abort(s_ota_handle);
    bool was_busy = s_ota_active || s_ota_pending;
    s_ota_active  = false;
    s_ota_pending = false;
    s_ota_handle  = 0;
    s_ota_part    = NULL;
    s_ota_size = s_ota_recv = s_ota_acked = s_ota_crc = s_ota_exp_crc = 0;
    if (fire_cb && was_busy && s_ota_cb) s_ota_cb(BLE_OTA_EV_FINISHED);
}

// Widen the BLE link's supervision timeout before the transfer. The slot erase
// is deferred into the chunk writes (OTA_WITH_SEQUENTIAL_WRITES in
// ble_config_ota_proceed), so there is no long radio-starving window left to
// outlast; the wider timeout is margin for per-sector erase/write stalls during
// the transfer, and the 15-30 ms interval keeps the data phase reasonably fast.
static void ota_widen_link(void)
{
    esp_ble_conn_update_params_t p = {0};
    memcpy(p.bda, s_peer_bda, sizeof(esp_bd_addr_t));
    p.min_int = 12;    // 15 ms (1.25 ms units)
    p.max_int = 24;    // 30 ms
    p.latency = 0;
    p.timeout = 800;   // 8 s (10 ms units), must outlast the ~4 s erase with margin
    esp_err_t e = esp_ble_gap_update_conn_params(&p);
    if (e) ESP_LOGW(TAG, "OTA conn-param update: %s", esp_err_to_name(e));
}

// OTACTL BEGIN: stash size/crc, widen the link, ask app_sm to quiesce. The real
// esp_ota_begin() runs later in ble_config_ota_proceed(), so no flash work can
// garble audio that is still live.
static void ota_handle_begin(const uint8_t *v, uint16_t len)
{
    if (len < 9) { ota_error(1, "short begin"); return; }
    ota_cleanup(false);
    s_ota_size     = get_u32(v + 1);
    s_ota_exp_crc  = get_u32(v + 5);
    s_ota_pending  = true;
    s_ota_touch_ms = now_ms();
    ESP_LOGI(TAG, "OTA begin requested: %u bytes (crc 0x%08x), quiescing audio",
             (unsigned)s_ota_size, (unsigned)s_ota_exp_crc);
    ota_widen_link();
    if (s_ota_cb) s_ota_cb(BLE_OTA_EV_BEGIN_REQUEST);
}

esp_err_t ble_config_ota_proceed(void)
{
    if (!s_ota_pending) return ESP_OK;   // request already timed out / aborted
    s_ota_pending = false;
    s_ota_part = esp_ota_get_next_update_partition(NULL);
    if (!s_ota_part) { ota_error(2, "no ota slot"); ota_cleanup(true); return ESP_FAIL; }
    ESP_LOGI(TAG, "OTA: %s ready for %u bytes", s_ota_part->label, (unsigned)s_ota_size);
    // OTA_WITH_SEQUENTIAL_WRITES, never s_ota_size: passing a size makes
    // esp_ota_begin() erase the whole slot up front (~4 s on a slot holding the
    // old image), starving the radio long enough for a central with a short
    // supervision timeout to drop the link before READY is sent -- BlueZ's
    // 720 ms default hits this even when ota_widen_link() has been requested,
    // since the central is not obliged to apply the new parameters in time.
    // The flag defers erasing to esp_ota_write(), one sector at a time,
    // interleaved with the incoming chunks. It requires writes in continuous
    // sequence, which the windowed protocol guarantees.
    esp_err_t e = esp_ota_begin(s_ota_part, OTA_WITH_SEQUENTIAL_WRITES, &s_ota_handle);
    if (e != ESP_OK) { ota_error(3, esp_err_to_name(e)); ota_cleanup(true); return e; }
    s_ota_active   = true;
    s_ota_recv = s_ota_acked = s_ota_crc = 0;
    s_ota_touch_ms = now_ms();
    uint32_t chunk = (s_mtu > 3) ? (uint32_t)(s_mtu - 3) : 20;
    if (chunk > OTA_CHUNK_MAX) chunk = OTA_CHUNK_MAX;
    uint8_t b[9]; b[0] = OTA_ST_READY; put_u32(b + 1, chunk); put_u32(b + 5, OTA_WINDOW);
    ota_notify(b, 9);
    ESP_LOGI(TAG, "OTA ready: chunk=%u window=%u", (unsigned)chunk, (unsigned)OTA_WINDOW);
    return ESP_OK;
}

// OTADATA: one raw firmware chunk.
static void ota_handle_data(const uint8_t *v, uint16_t len)
{
    if (!s_ota_active) return;
    esp_err_t e = esp_ota_write(s_ota_handle, v, len);
    if (e != ESP_OK) { ota_error(4, esp_err_to_name(e)); ota_cleanup(true); return; }
    s_ota_crc      = esp_rom_crc32_le(s_ota_crc, v, len);
    s_ota_recv    += len;
    s_ota_touch_ms = now_ms();
    if (s_ota_recv - s_ota_acked >= OTA_WINDOW / 2 || s_ota_recv >= s_ota_size) {
        s_ota_acked = s_ota_recv;
        uint8_t b[5]; b[0] = OTA_ST_ACK; put_u32(b + 1, s_ota_acked);
        ota_notify(b, 5);
    }
}

// OTACTL END: finalize (validates header + appended SHA-256), swap boot slot, reboot.
static void ota_handle_end(void)
{
    if (!s_ota_active) { ota_error(5, "not active"); return; }
    if (s_ota_recv != s_ota_size)
        ESP_LOGW(TAG, "OTA size mismatch: got %u want %u",
                 (unsigned)s_ota_recv, (unsigned)s_ota_size);
    if (s_ota_crc != s_ota_exp_crc)
        ESP_LOGW(TAG, "OTA crc32 0x%08x != 0x%08x (SHA-256 still enforced below)",
                 (unsigned)s_ota_crc, (unsigned)s_ota_exp_crc);
    esp_err_t e = esp_ota_end(s_ota_handle);
    s_ota_handle = 0;
    if (e != ESP_OK) { s_ota_active = false; ota_error(6, esp_err_to_name(e)); ota_cleanup(true); return; }
    e = esp_ota_set_boot_partition(s_ota_part);
    if (e != ESP_OK) { s_ota_active = false; ota_error(7, esp_err_to_name(e)); ota_cleanup(true); return; }
    s_ota_active = false;
    ESP_LOGW(TAG, "OTA complete -> boot %s; rebooting", s_ota_part->label);
    uint8_t done = OTA_ST_DONE; ota_notify(&done, 1);
    vTaskDelay(pdMS_TO_TICKS(400));   // let the DONE notify flush before reset
    esp_restart();
}

// OTACTL GET_INFO: reply "<version>|<running-slot>|<build-date>" so the UI can
// show the installed firmware version (CONFIG_APP_PROJECT_VER) + which slot it
// runs from + when it was built (the date distinguishes dev builds at the same
// pinned version).
static void ota_handle_get_info(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_app_desc_t  *d   = esp_app_get_description();
    uint8_t b[64];
    b[0] = OTA_ST_INFO;
    int n = snprintf((char *)b + 1, sizeof(b) - 1, "%s|%s|%s",
                     d ? d->version : "?", run ? run->label : "?", d ? d->date : "");
    if (n < 0) n = 0;
    if (n > (int)sizeof(b) - 1) n = (int)sizeof(b) - 1;
    ota_notify(b, (uint16_t)(1 + n));
}

static void ota_handle_ctl(const uint8_t *v, uint16_t len)
{
    if (len < 1) return;
    switch (v[0]) {
    case OTA_OP_BEGIN:    ota_handle_begin(v, len); break;
    case OTA_OP_END:      ota_handle_end();         break;
    case OTA_OP_ABORT:    ESP_LOGW(TAG, "OTA aborted by client"); ota_cleanup(true); break;
    case OTA_OP_GET_INFO: ota_handle_get_info();    break;
    default: ESP_LOGW(TAG, "OTA ctl: unknown op 0x%02x", v[0]); break;
    }
}

void ble_config_set_ota_cb(ble_ota_event_cb_t cb) { s_ota_cb = cb; }

// ---- BLE debug console ----------------------------------------------------
// ble_log_vprintf tees every ESP_LOG line into s_log_sb; ble_log_task drains it
// to LOG notifications. The UART logger still runs (we chain to the previous
// vprintf), so the cable path is unchanged. CMD writes are queued and run by
// ble_cmd_task via esp_console_run(), with the command's stdout captured (a
// per-task stdout swap) and teed back over the same LOG stream.

// True while there's enough free internal heap to send a BLE notification
// without risking the A2DP SBC encoder's buffers. Gates every notify path.
static inline bool ble_heap_ok(void)
{
    return esp_get_free_internal_heap_size() > BLE_NOTIFY_HEAP_FLOOR;
}

// Enqueue raw bytes for the LOG notify stream. No-op unless a client is
// subscribed, so nothing accumulates when nobody is listening. Also bails under
// heap pressure so the log tee can't starve audio (and can't feed a spiral).
static void ble_log_push(const char *data, size_t len)
{
    if (!s_log_sb || !s_connected || !s_log_notify_on || len == 0) return;
    if (!ble_heap_ok()) return;
    xStreamBufferSend(s_log_sb, data, len, 0);   // non-blocking; drops if full
}
static void ble_log_str(const char *s) { ble_log_push(s, strlen(s)); }

// Log hook: forward to the previous (UART) logger, then tee into the BLE stream.
// Skips logs emitted by our own drain task so a notify-path log can't feed back.
static int ble_log_vprintf(const char *fmt, va_list ap)
{
    va_list ap2; va_copy(ap2, ap);
    int n = s_prev_vprintf ? s_prev_vprintf(fmt, ap) : vprintf(fmt, ap);
    if (s_log_sb && s_connected && s_log_notify_on &&
        xTaskGetCurrentTaskHandle() != s_log_task) {
        char buf[192];
        int m = vsnprintf(buf, sizeof(buf), fmt, ap2);
        if (m > 0) ble_log_push(buf, (size_t)(m < (int)sizeof(buf) ? m : (int)sizeof(buf)));
    }
    va_end(ap2);
    return n;
}

static void ble_log_task(void *arg)
{
    (void)arg;
    static uint8_t buf[BLE_LOG_VAL_MAX];
    for (;;) {
        // Finite timeout so a sub-trigger-level burst (e.g. a short command
        // reply) still flushes promptly instead of waiting for more bytes.
        size_t n = xStreamBufferReceive(s_log_sb, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (!n) continue;
        uint16_t chunk = (s_mtu > 3) ? (uint16_t)(s_mtu - 3) : 20;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);
        for (size_t off = 0; off < n; ) {
            if (!s_connected || !s_log_notify_on || !ble_heap_ok()) break;  // gone or heap-tight; drop the rest
            uint16_t send = (uint16_t)((n - off > chunk) ? chunk : (n - off));
            esp_err_t e = esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id,
                              s_handles[IDX_LOG_VAL], send, buf + off, false);
            if (e != ESP_OK) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }  // controller busy; retry
            off += send;
        }
    }
}

// Run one console line through the shared esp_console command table, capturing
// its stdout so the result streams back over LOG. stdout is per-task in IDF's
// newlib, so swapping it here redirects only this task (the UART REPL keeps its
// own). Runs in ble_cmd_task, never in the GATTS callback.
static void ble_run_cmd(char *line)
{
    char echo[BLE_CMD_VAL_MAX + 8];
    int en = snprintf(echo, sizeof(echo), "hifi> %s\n", line);
    if (en > 0) ble_log_push(echo, (size_t)(en < (int)sizeof(echo) ? en : (int)sizeof(echo)));

    char  *out = NULL; size_t outsz = 0;
    FILE  *mem = open_memstream(&out, &outsz);
    FILE  *saved = stdout;
    if (mem) stdout = mem;
    int ret = 0;
    esp_err_t e = esp_console_run(line, &ret);
    if (mem) { fflush(mem); stdout = saved; fclose(mem); }

    if (out && outsz) ble_log_push(out, outsz);
    if (e == ESP_ERR_NOT_FOUND) {
        ble_log_str("unknown command (try 'help')\n");
    } else if (e != ESP_OK && e != ESP_ERR_INVALID_ARG) {  // INVALID_ARG = empty line
        char b[64];
        int n = snprintf(b, sizeof(b), "error: %s\n", esp_err_to_name(e));
        if (n > 0) ble_log_push(b, (size_t)n);
    }
    free(out);
}

static void ble_cmd_task(void *arg)
{
    (void)arg;
    char *line;
    for (;;) {
        if (xQueueReceive(s_cmd_q, &line, portMAX_DELAY) == pdTRUE) {
            ble_run_cmd(line);
            free(line);
        }
    }
}

// Compute + notify the stereo audio spectrum at ~20 fps while a client watches.
// The FFT (audio_pipeline_compute_spectrum) runs here, not in the audio task.
static void ble_spec_task(void *arg)
{
    (void)arg;
    uint8_t frame[1 + 2 * SPEC_BINS];   // [0]=source (0 speaker, 1 BT, 2 headphone), then L bands, R bands
    for (;;) {
        // Skip while nobody's watching, and yield to audio when heap is tight so
        // the visualizer can never starve an active A2DP stream.
        if (!(s_connected && s_spec_notify_on) || !ble_heap_ok()) {
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;
        }
        int n = audio_pipeline_compute_spectrum(frame + 1, SPEC_BINS);
        if (n > 0) {
            frame[0] = audio_pipeline_spectrum_is_bt() ? 1 : (app_sm_hp_plugged() ? 2 : 0);
            esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id, s_handles[IDX_SPEC_VAL],
                                        (uint16_t)(1 + n), frame, false /* notify */);
        }
        vTaskDelay(pdMS_TO_TICKS(SPEC_INTERVAL_MS));
    }
}

// ---- notify-on-change -----------------------------------------------------
// Poll the settings generation; push the current blob to a subscribed central
// whenever it changes. This catches edits from either control surface (BLE write
// or the UART console) so the web UI stays live without re-reading. Also hosts
// the OTA stall watchdog (cheap; runs on the same 750 ms tick).
static void notify_timer_cb(void *arg)
{
    (void)arg;
    // OTA stall watchdog: a transfer (or a BEGIN that app_sm never proceeded on)
    // must not wedge the OTA state forever (the production board has no
    // accessible reset). Abort if idle too long.
    if ((s_ota_active || s_ota_pending) && now_ms() - s_ota_touch_ms > OTA_STALL_MS) {
        ESP_LOGW(TAG, "OTA stalled %d ms -> abort", OTA_STALL_MS);
        ota_error(8, "stalled");
        ota_cleanup(true);
    }
    if (!s_connected || !s_notify_on) return;
    uint32_t gen = settings_generation();
    if (gen == s_last_gen) return;
    s_last_gen = gen;
    uint8_t buf[SETTINGS_WIRE_LEN];
    settings_to_wire(buf);
    esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id, s_handles[IDX_SETTINGS_VAL],
                                sizeof(buf), buf, false /* notify, no confirm */);
}

// ---- advertising ----------------------------------------------------------
// Name in the adv payload (Web Bluetooth filters by name), 128-bit service UUID
// in the scan response (doesn't fit alongside the name in 31 bytes).
static esp_ble_adv_data_t s_adv_data = {
    .set_scan_rsp    = false,
    .include_name    = true,
    .include_txpower = true,
    .min_interval    = 0x0006,
    .max_interval    = 0x0010,
    .flag            = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};
static esp_ble_adv_data_t s_scan_rsp_data = {
    .set_scan_rsp     = true,
    .service_uuid_len = sizeof(SVC_UUID128),
    .p_service_uuid   = (uint8_t *)SVC_UUID128,
};
// Advertising interval (units of 0.625 ms). Deliberately slow: the config UI is
// used occasionally, and a fast 20-40 ms interval would cost ~10 mA continuously
// on the GBA's switched battery rail. At ~1.5-2 s the advertising duty cycle
// drops ~40x, pulling that well under 1 mA while staying connectable any time
// (cost is a ~1-3 s discovery delay while the browser catches an advertisement).
// The GATT server stays up, so re-tuning never needs a button gesture or reboot.
#define BLE_ADV_INT_MIN  0x0960   // 2400 x 0.625 ms = 1.5 s
#define BLE_ADV_INT_MAX  0x0C80   // 3200 x 0.625 ms = 2.0 s
static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min       = BLE_ADV_INT_MIN,
    .adv_int_max       = BLE_ADV_INT_MAX,
    .adv_type          = ADV_TYPE_IND,
    .own_addr_type     = BLE_ADDR_TYPE_PUBLIC,
    .channel_map       = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};
static bool s_adv_cfg_done  = false;
static bool s_scan_rsp_done = false;
static bool s_adv_inhibit   = false;  // `radio off`: suppress all advertising

static void start_adv_if_ready(void)
{
    if (s_adv_cfg_done && s_scan_rsp_done && !s_adv_inhibit) {
        esp_ble_gap_start_advertising(&s_adv_params);
    }
}

void ble_config_set_advertising(bool on)
{
    s_adv_inhibit = !on;
    if (on) esp_ble_gap_start_advertising(&s_adv_params);
    else    esp_ble_gap_stop_advertising();
    ESP_LOGI(TAG, "BLE advertising %s", on ? "on" : "off");
}

void ble_config_set_adv_interval_ms(uint32_t ms)
{
    // Clamp to the BLE spec range (20 ms .. 10.24 s), keeping room for the
    // +10 ms min/max jitter window the controller wants.
    if (ms < 20)    ms = 20;
    if (ms > 10220) ms = 10220;
    uint16_t units = (uint16_t)((ms * 1000) / 625);
    s_adv_params.adv_int_min = units;
    s_adv_params.adv_int_max = units + 16;
    esp_ble_gap_stop_advertising();
    start_adv_if_ready();
    ESP_LOGI(TAG, "BLE adv interval %u ms (%u units)", (unsigned)ms, units);
}

void ble_config_set_tx_power_dbm(int dbm)
{
    if (dbm < -12) dbm = -12;
    if (dbm > 9)   dbm = 9;
    // Controller steps are 3 dB apart from N12 (=0) to P9 (=7); snap down.
    esp_power_level_t lvl = (esp_power_level_t)(ESP_PWR_LVL_N12 + (dbm + 12) / 3);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV,       lvl);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_CONN_HDL0, lvl);
    ESP_LOGI(TAG, "BLE TX power %d dBm (lvl %d)", dbm, (int)lvl);
}

// ---- GAP (BLE) callback ---------------------------------------------------
static void gap_ble_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        s_adv_cfg_done = true;
        start_adv_if_ready();
        break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        s_scan_rsp_done = true;
        start_adv_if_ready();
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "adv start failed: %d", param->adv_start_cmpl.status);
        } else {
            ESP_LOGI(TAG, "advertising as \"%s\"", BLE_DEVICE_NAME);
        }
        break;
    default:
        break;
    }
}

// ---- GATTS callback -------------------------------------------------------
static void gatts_cb(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                     esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTS_REG_EVT:
        s_gatts_if = gatts_if;
        esp_ble_gap_set_device_name(BLE_DEVICE_NAME);
        esp_ble_gap_config_adv_data(&s_adv_data);
        esp_ble_gap_config_adv_data(&s_scan_rsp_data);
        esp_ble_gatts_create_attr_tab(k_gatt_db, gatts_if, IDX_NB, 0);
        break;

    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
        if (param->add_attr_tab.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "attr tab failed: 0x%x", param->add_attr_tab.status);
        } else if (param->add_attr_tab.num_handle != IDX_NB) {
            ESP_LOGE(TAG, "attr tab handle count %d != %d",
                     param->add_attr_tab.num_handle, IDX_NB);
        } else {
            memcpy(s_handles, param->add_attr_tab.handles, sizeof(s_handles));
            esp_ble_gatts_start_service(s_handles[IDX_SVC]);
            ESP_LOGI(TAG, "service started (settings handle=%d, action handle=%d)",
                     s_handles[IDX_SETTINGS_VAL], s_handles[IDX_ACTION_VAL]);
        }
        break;

    case ESP_GATTS_CONNECT_EVT:
        s_conn_id   = param->connect.conn_id;
        memcpy(s_peer_bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
        s_connected = true;
        s_log_notify_on = false;   // client re-subscribes to LOG after connect
        s_spec_notify_on = false;  // and to SPECTRUM
        s_mtu       = 23;   // reset to the ATT default until MTU_EVT renegotiates
        s_last_gen  = settings_generation();
        ESP_LOGI(TAG, "BLE central connected, conn_id=%d", s_conn_id);
        break;

    case ESP_GATTS_MTU_EVT:
        // Web Bluetooth negotiates ~517; remember it so OTA picks the largest
        // safe chunk size (MTU-3) for the READY reply.
        s_mtu = param->mtu.mtu;
        ESP_LOGI(TAG, "ATT MTU = %u", s_mtu);
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        ESP_LOGI(TAG, "BLE central disconnected (reason=0x%x), re-advertising",
                 param->disconnect.reason);
        s_connected = false;
        s_notify_on = false;
        s_log_notify_on = false;
        s_spec_notify_on = false;
        audio_pipeline_spectrum_enable(false);   // stop the FFT tap when nobody's watching
        // Abort any in-flight OTA and let app_sm resume the audio path.
        ota_cleanup(true);
        if (!s_adv_inhibit) esp_ble_gap_start_advertising(&s_adv_params);
        break;

    case ESP_GATTS_READ_EVT:
        if (param->read.handle == s_handles[IDX_SETTINGS_VAL]) {
            esp_gatt_rsp_t rsp = {0};
            rsp.attr_value.handle = param->read.handle;
            rsp.attr_value.len    = SETTINGS_WIRE_LEN;
            settings_to_wire(rsp.attr_value.value);
            esp_ble_gatts_send_response(gatts_if, param->read.conn_id,
                                        param->read.trans_id, ESP_GATT_OK, &rsp);
        }
        break;

    case ESP_GATTS_WRITE_EVT:
        if (param->write.is_prep) {
            // No long writes in this schema; ack so the central isn't stuck.
            if (param->write.need_rsp) {
                esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                            param->write.trans_id, ESP_GATT_OK, NULL);
            }
            break;
        }
        if (param->write.handle == s_handles[IDX_SETTINGS_VAL]) {
            if (param->write.len == SETTINGS_WIRE_LEN) {
                wire_to_settings(param->write.value);
                ESP_LOGI(TAG, "settings write applied (vol=%u btvol=%u)",
                         param->write.value[2], param->write.value[3]);
            } else {
                ESP_LOGW(TAG, "settings write wrong len %d (want %d)",
                         param->write.len, SETTINGS_WIRE_LEN);
            }
            if (param->write.need_rsp) {
                esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                            param->write.trans_id, ESP_GATT_OK, NULL);
            }
        } else if (param->write.handle == s_handles[IDX_ACTION_VAL]) {
            do_action(param->write.value, param->write.len);
            if (param->write.need_rsp) {
                esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                            param->write.trans_id, ESP_GATT_OK, NULL);
            }
        } else if (param->write.handle == s_handles[IDX_OTACTL_VAL]) {
            // AUTO_RSP: the stack acked already; just process the control op.
            ota_handle_ctl(param->write.value, param->write.len);
        } else if (param->write.handle == s_handles[IDX_OTADATA_VAL]) {
            // Write-without-response firmware chunk; no ack to send.
            ota_handle_data(param->write.value, param->write.len);
        } else if (param->write.handle == s_handles[IDX_CMD_VAL]) {
            // Console command line (AUTO_RSP: the stack already acked). Copy,
            // trim trailing CR/LF/space, and hand to ble_cmd_task; running the
            // command in this (Bluedroid) callback context is unsafe.
            uint16_t n = param->write.len;
            if (n > BLE_CMD_VAL_MAX) n = BLE_CMD_VAL_MAX;
            char *line = malloc((size_t)n + 1);
            if (line) {
                memcpy(line, param->write.value, n);
                line[n] = '\0';
                while (n && (line[n-1] == '\n' || line[n-1] == '\r' || line[n-1] == ' '))
                    line[--n] = '\0';
                if (n == 0 || !s_cmd_q || xQueueSend(s_cmd_q, &line, 0) != pdTRUE) free(line);
            }
        } else if (param->write.handle == s_handles[IDX_LOG_CCCD]) {
            if (param->write.len >= 2) {
                s_log_notify_on = (param->write.value[0] & 0x01) != 0;
                ESP_LOGI(TAG, "log notify %s", s_log_notify_on ? "subscribed" : "off");
            }
        } else if (param->write.handle == s_handles[IDX_SPEC_CCCD]) {
            if (param->write.len >= 2) {
                s_spec_notify_on = (param->write.value[0] & 0x01) != 0;
                audio_pipeline_spectrum_enable(s_spec_notify_on);  // gate the FFT tap
                ESP_LOGI(TAG, "spectrum %s", s_spec_notify_on ? "on" : "off");
            }
        } else if (param->write.handle == s_handles[IDX_SETTINGS_CCCD]) {
            // CCCD is AUTO_RSP (stack stores the value + sends the ack); we only
            // read it to track whether to push notifications.
            if (param->write.len >= 2) {
                s_notify_on = (param->write.value[0] & 0x01) != 0;
                s_last_gen  = settings_generation();
                ESP_LOGI(TAG, "notify %s", s_notify_on ? "subscribed" : "off");
            }
        }
        break;

    default:
        break;
    }
}

esp_err_t ble_config_init(void)
{
    esp_err_t err = esp_ble_gatts_register_callback(gatts_cb);
    if (err) { ESP_LOGE(TAG, "gatts cb reg: %s", esp_err_to_name(err)); return err; }
    err = esp_ble_gap_register_callback(gap_ble_cb);
    if (err) { ESP_LOGE(TAG, "gap cb reg: %s", esp_err_to_name(err)); return err; }
    err = esp_ble_gatts_app_register(BLE_APP_ID);
    if (err) { ESP_LOGE(TAG, "gatts app reg: %s", esp_err_to_name(err)); return err; }

    // Raise the local MTU to the max so (a) the SETTINGS_WIRE_LEN (28-byte)
    // settings read/write fits one PDU (the default 23-byte MTU would force a
    // multi-PDU Read-Blob the RSP_BY_APP handler doesn't implement), and (b) OTA
    // streams ~500-byte chunks instead of ~20 (a 1 MB image over 20-byte writes
    // would crawl). Web Bluetooth negotiates ~517; we pick chunk = MTU-3 at READY.
    err = esp_ble_gatt_set_local_mtu(517);
    if (err) ESP_LOGW(TAG, "set_local_mtu: %s", esp_err_to_name(err));

    // -3 dBm BLE TX matches the BR/EDR setting: a config phone is within a metre
    // and the lower power keeps RF noise off the analog front end.
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV,       ESP_PWR_LVL_N3);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_CONN_HDL0, ESP_PWR_LVL_N3);

    // Poll the settings generation and notify subscribers on change (catches
    // both BLE-write and UART-console edits). Cheap: a compare every 750 ms,
    // and it early-returns unless a central is connected AND subscribed.
    const esp_timer_create_args_t targs = {
        .callback = notify_timer_cb,
        .name     = "ble_notify",
    };
    err = esp_timer_create(&targs, &s_notify_timer);
    if (err) { ESP_LOGE(TAG, "notify timer: %s", esp_err_to_name(err)); return err; }
    esp_timer_start_periodic(s_notify_timer, 750 * 1000);

    // BLE debug console: a LOG notify stream fed by an ESP_LOG vprintf hook, and
    // a CMD write dispatched through the shared esp_console table. Additive to
    // the UART console (the hook chains to the previous logger).
    // Trigger level 1: wake the drain on any buffered byte. It reads up to a
    // full notify payload per pass, so bursts still coalesce; small replies flush.
    s_log_sb = xStreamBufferCreate(BLE_LOG_SB_SIZE, 1);
    if (s_log_sb) {
        xTaskCreate(ble_log_task, "ble_log", 3072, NULL, 5, &s_log_task);
    } else {
        ESP_LOGW(TAG, "LOG stream buffer alloc failed; BLE log disabled");
    }
    s_cmd_q = xQueueCreate(4, sizeof(char *));
    if (s_cmd_q) xTaskCreate(ble_cmd_task, "ble_cmd", 4096, NULL, 5, NULL);
    s_prev_vprintf = esp_log_set_vprintf(ble_log_vprintf);

    // Audio spectrum notifier (gated on the SPECTRUM CCCD; idle otherwise). FFT
    // scratch is static, so the stack stays small.
    xTaskCreate(ble_spec_task, "ble_spec", 3072, NULL, 4, &s_spec_task);

    ESP_LOGI(TAG, "BLE GATT config server init (settings + action + log/cmd chars)");
    return ESP_OK;
}

#else  // !CONFIG_GBHIFI_BLE_CONFIG

esp_err_t ble_config_init(void) { return ESP_OK; }
void      ble_config_set_ota_cb(ble_ota_event_cb_t cb) { (void)cb; }
esp_err_t ble_config_ota_proceed(void) { return ESP_OK; }
void      ble_config_set_adv_interval_ms(uint32_t ms) { (void)ms; }
void      ble_config_set_tx_power_dbm(int dbm) { (void)dbm; }

#endif
