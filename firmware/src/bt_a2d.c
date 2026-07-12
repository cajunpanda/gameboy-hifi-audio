#include "bt_a2d.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio_pipeline.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "bt_a2d";

#define LOCAL_DEVICE_NAME "GameBoy HiFi"

// Cap on how many bonded sinks we track / cycle through when reconnecting.
#define MAX_BONDED 4

// Cap on the per-session pairing failure blacklist. Any inquiry-matched sink
// that fails to open A2DP is remembered here for the rest of the pairing
// session and skipped on subsequent inquiries, so a device that passes the
// class-of-device filter but refuses the source connection (e.g. a nearby TV
// sharing a speaker's exact CoD) can't win every re-scan and starve the sink
// the user actually wants. Rebuilt from scratch each session, so it adapts to
// whatever environment the mod is powered up in — nothing is persisted.
#define MAX_FAIL_BL 8

// NVS record of the most-recently-connected sink, so reconnect tries it first.
#define NVS_NS       "gbhifi"
#define NVS_KEY_LAST "last_bda"

// ---- application work dispatcher ------------------------------------------
// A2DP and GAP callbacks fire on the BT controller task and must not block;
// dispatch each through this queue so handlers run in a normal FreeRTOS
// context where they can safely call IDF APIs and our event callback.

typedef void (*bt_work_cb_t)(uint16_t event, void *param);

typedef struct {
    bt_work_cb_t cb;
    uint16_t     event;
    void        *param;
} bt_work_msg_t;

static QueueHandle_t s_work_q  = NULL;
static TaskHandle_t  s_work_th = NULL;
static bool          s_inited  = false;

static void bt_work_task(void *arg)
{
    bt_work_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_work_q, &msg, portMAX_DELAY) == pdTRUE) {
            if (msg.cb) {
                msg.cb(msg.event, msg.param);
            }
            if (msg.param) {
                free(msg.param);
            }
        }
    }
}

static bool bt_work_dispatch(bt_work_cb_t cb, uint16_t event,
                             const void *params, int params_len)
{
    bt_work_msg_t msg = { .cb = cb, .event = event, .param = NULL };
    if (params_len > 0 && params != NULL) {
        msg.param = malloc(params_len);
        if (!msg.param) {
            return false;
        }
        memcpy(msg.param, params, params_len);
    }
    if (xQueueSend(s_work_q, &msg, pdMS_TO_TICKS(10)) != pdTRUE) {
        if (msg.param) free(msg.param);
        ESP_LOGE(TAG, "work queue full");
        return false;
    }
    return true;
}

// ---- event publish --------------------------------------------------------

static bt_event_cb_t s_event_cb = NULL;

void bt_a2d_set_event_cb(bt_event_cb_t cb)
{
    s_event_cb = cb;
}

static void publish(bt_event_t type, const esp_bd_addr_t bda)
{
    if (!s_event_cb) return;
    bt_event_msg_t m = { .type = type };
    if (bda) {
        memcpy(m.bda, bda, ESP_BD_ADDR_LEN);
    }
    s_event_cb(&m);
}

// ---- runtime state --------------------------------------------------------

enum {
    LINK_IDLE,
    LINK_DISCOVERING,
    LINK_PAGING,
    LINK_CONNECTED,
};

enum {
    MEDIA_IDLE,
    MEDIA_STARTING,
    MEDIA_STARTED,
    MEDIA_SUSPENDING,
};

static int           s_link_state    = LINK_IDLE;
static int           s_media_state   = MEDIA_IDLE;
// Tracks the baseband ACL link, which outlives the A2DP profile: on teardown
// A2DP disconnects first and the ACL link drops a beat later. Mode A must not
// hand-roll light sleep until the ACL is fully down (see bt_a2d_link_active),
// so we follow the GAP ACL conn/disconn-complete events, not the A2DP state.
static bool          s_acl_up        = false;
static esp_bd_addr_t s_peer_bda      = {0};
static bool          s_pairing_mode  = false;  // true while inquiry-pairing
static bool          s_radio_off     = false;  // `radio off`: gate inquiry + paging TX
// Cursor into the most-recent-first ordered bond list, advanced on each
// reconnect attempt so successive ticks cycle through every bonded sink.
static int           s_try_idx       = 0;
// Per-session pairing failure blacklist (see MAX_FAIL_BL). Cleared at the
// start of each pairing session by bt_a2d_reset_fail_blacklist().
static esp_bd_addr_t s_fail_bl[MAX_FAIL_BL];
static int           s_fail_bl_n     = 0;

static bool fail_bl_contains(const esp_bd_addr_t bda)
{
    for (int i = 0; i < s_fail_bl_n; i++) {
        if (memcmp(s_fail_bl[i], bda, ESP_BD_ADDR_LEN) == 0) return true;
    }
    return false;
}

static void fail_bl_add(const esp_bd_addr_t bda)
{
    if (fail_bl_contains(bda)) return;
    if (s_fail_bl_n >= MAX_FAIL_BL) {
        // Full (many failing devices in range): drop the oldest so the most
        // recent failures still get skipped. Bounded churn, never overflows.
        memmove(s_fail_bl[0], s_fail_bl[1],
                (MAX_FAIL_BL - 1) * ESP_BD_ADDR_LEN);
        s_fail_bl_n = MAX_FAIL_BL - 1;
    }
    memcpy(s_fail_bl[s_fail_bl_n++], bda, ESP_BD_ADDR_LEN);
}

// ---- A2DP data callback ---------------------------------------------------
// Pulls 16-bit interleaved stereo PCM out of the audio_pipeline stream
// buffer to feed the SBC encoder. Pads with zeros on underrun so the
// encoder keeps its frame cadence. Rate is locked at 44.1 kHz: the IDF
// A2DP source endpoint only accepts that rate.

static int32_t a2d_data_cb(uint8_t *buf, int32_t len)
{
    if (!buf || len <= 0) return 0;
    size_t got = audio_pipeline_read_stereo16(buf, (size_t)len, 0);
    if (got < (size_t)len) {
        memset(buf + got, 0, (size_t)len - got);
    }
    return len;
}

// ---- helpers --------------------------------------------------------------

static void bda_str(const esp_bd_addr_t bda, char *out)
{
    sprintf(out, "%02x:%02x:%02x:%02x:%02x:%02x",
            bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

static bool eir_name(const uint8_t *eir, char *out, size_t out_size)
{
    if (!eir) return false;
    uint8_t name_len = 0;
    uint8_t *name = esp_bt_gap_resolve_eir_data((uint8_t *)eir,
                                                ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME,
                                                &name_len);
    if (!name) {
        name = esp_bt_gap_resolve_eir_data((uint8_t *)eir,
                                           ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME,
                                           &name_len);
    }
    if (!name) return false;
    if (name_len >= out_size) name_len = out_size - 1;
    memcpy(out, name, name_len);
    out[name_len] = '\0';
    return true;
}

// ---- NVS: most-recently-connected sink ------------------------------------

static bool load_last_bda(esp_bd_addr_t out)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = ESP_BD_ADDR_LEN;
    esp_err_t e = nvs_get_blob(h, NVS_KEY_LAST, out, &len);
    nvs_close(h);
    return e == ESP_OK && len == ESP_BD_ADDR_LEN;
}

static void save_last_bda(const esp_bd_addr_t bda)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_blob(h, NVS_KEY_LAST, bda, ESP_BD_ADDR_LEN) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

// ---- inquiry handling -----------------------------------------------------

static void handle_inquiry_result(esp_bt_gap_cb_param_t *p)
{
    char addr[18];
    bda_str(p->disc_res.bda, addr);

    uint32_t cod = 0;
    const uint8_t *eir = NULL;
    for (int i = 0; i < p->disc_res.num_prop; i++) {
        esp_bt_gap_dev_prop_t *prop = &p->disc_res.prop[i];
        if (prop->type == ESP_BT_GAP_DEV_PROP_COD) {
            cod = *(uint32_t *)prop->val;
        } else if (prop->type == ESP_BT_GAP_DEV_PROP_EIR) {
            eir = (uint8_t *)prop->val;
        }
    }

    char name[ESP_BT_GAP_MAX_BDNAME_LEN + 1] = {0};
    bool have_name = eir_name(eir, name, sizeof(name));
    ESP_LOGI(TAG, "scan: %s  cod=0x%06" PRIx32 "%s%s",
             addr, cod,
             have_name ? "  name=" : "",
             have_name ? name : "");

    // Match dedicated audio hardware: major device class Audio/Video AND an
    // audio-ish service bit (Rendering or Audio). No name filter: consent comes
    // from the user explicitly entering pairing mode, and the first matching
    // sink to answer the inquiry wins.
    //
    // Both halves matter. Rendering-service alone also matches discoverable
    // computers (a bench laptop, CoD 0x6c010c, major class Computer, sits there
    // winning every inquiry and rejecting auth). And Rendering alone MISSES
    // some real speakers: a bench BT loudspeaker advertises CoD 0x280424 --
    // major AV, minor loudspeaker, services Audio+Capturing, no Rendering bit.
    if (!esp_bt_gap_is_valid_cod(cod) ||
        esp_bt_gap_get_cod_major_dev(cod) != ESP_BT_COD_MAJOR_DEV_AV ||
        !(esp_bt_gap_get_cod_srvc(cod) &
          (ESP_BT_COD_SRVC_RENDERING | ESP_BT_COD_SRVC_AUDIO))) {
        return;
    }

    // Skip a sink that already failed to open A2DP this session. Don't cancel
    // discovery — let the inquiry keep running so a different, working sink can
    // still answer and win.
    if (fail_bl_contains(p->disc_res.bda)) {
        ESP_LOGI(TAG, "skip blacklisted (prev A2DP fail) %s", addr);
        return;
    }

    ESP_LOGI(TAG, "matched sink: %s name=%s", addr,
             have_name ? name : "(unknown)");
    memcpy(s_peer_bda, p->disc_res.bda, ESP_BD_ADDR_LEN);
    esp_bt_gap_cancel_discovery();
}

// ---- GAP callback ---------------------------------------------------------

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT:
        if (s_link_state == LINK_DISCOVERING) {
            handle_inquiry_result(param);
        }
        break;
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED &&
            s_link_state == LINK_DISCOVERING) {
            // Did handle_inquiry_result record a peer?
            bool have_peer = false;
            for (int i = 0; i < ESP_BD_ADDR_LEN; i++) {
                if (s_peer_bda[i]) { have_peer = true; break; }
            }
            if (have_peer) {
                char addr[18];
                bda_str(s_peer_bda, addr);
                ESP_LOGI(TAG, "paging matched sink %s", addr);
                s_link_state = LINK_PAGING;
                esp_a2d_source_connect(s_peer_bda);
            } else {
                ESP_LOGI(TAG, "pairing inquiry: no matching sink found");
                s_link_state = LINK_IDLE;
                s_pairing_mode = false;
                publish(BT_EV_PAIRING_TIMEOUT, NULL);
            }
        }
        break;
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "auth ok with %s", param->auth_cmpl.device_name);
        } else {
            ESP_LOGE(TAG, "auth failed: %d", param->auth_cmpl.stat);
        }
        break;
    case ESP_BT_GAP_CFM_REQ_EVT:
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGI(TAG, "mode change: %d", param->mode_chg.mode);
        break;
    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
        s_acl_up = true;
        ESP_LOGI(TAG, "ACL up (hdl 0x%x)", param->acl_conn_cmpl_stat.handle);
        break;
    case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
        // Baseband link fully down. Trails the A2DP DISCONNECTED event on
        // teardown; this is the point at which Mode A's manual light sleep is
        // safe (see bt_a2d_link_active / the Mode A BT-quiesce wait).
        s_acl_up = false;
        ESP_LOGI(TAG, "ACL down (hdl 0x%x, reason 0x%x)",
                 param->acl_disconn_cmpl_stat.handle,
                 param->acl_disconn_cmpl_stat.reason);
        break;
    default:
        break;
    }
}

// ---- A2DP callback (deferred via work queue) ------------------------------

static void av_event_hdlr(uint16_t event, void *param)
{
    esp_a2d_cb_param_t *a2d = (esp_a2d_cb_param_t *)param;
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
        if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            char addr[18];
            bda_str(a2d->conn_stat.remote_bda, addr);
            ESP_LOGI(TAG, "*** A2DP connected to %s ***", addr);
            memcpy(s_peer_bda, a2d->conn_stat.remote_bda, ESP_BD_ADDR_LEN);
            s_link_state   = LINK_CONNECTED;
            s_media_state  = MEDIA_IDLE;
            s_pairing_mode = false;
            // Remember this sink as most-recent so the next reconnect
            // campaign pages it first, and reset the round-robin cursor.
            save_last_bda(s_peer_bda);
            s_try_idx = 0;
            // -3 dBm is plenty for a headphone within a meter; the default
            // +9 dBm just burns battery and adds noise. Set min and max to
            // the same level so the controller doesn't auto-ramp.
            esp_err_t pe = esp_bredr_tx_power_set(ESP_PWR_LVL_N3, ESP_PWR_LVL_N3);
            if (pe == ESP_OK) {
                ESP_LOGI(TAG, "BR/EDR TX power set to -3 dBm");
            } else {
                ESP_LOGW(TAG, "tx_power_set: %s", esp_err_to_name(pe));
            }
            publish(BT_EV_CONNECTED, s_peer_bda);
        } else if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGI(TAG, "A2DP disconnected");
            // A failed page during pairing never reached LINK_CONNECTED, so the
            // link is still LINK_PAGING here (a genuine post-connect drop would
            // be LINK_CONNECTED). Blacklist that peer for the rest of the
            // session so the next inquiry doesn't just re-match it and loop.
            if (s_pairing_mode && s_link_state == LINK_PAGING) {
                char addr[18];
                bda_str(a2d->conn_stat.remote_bda, addr);
                fail_bl_add(a2d->conn_stat.remote_bda);
                ESP_LOGW(TAG, "sink %s failed A2DP open; blacklisted for session (%d)",
                         addr, s_fail_bl_n);
            }
            s_link_state  = LINK_IDLE;
            s_media_state = MEDIA_IDLE;
            audio_pipeline_set_bt_streaming(false);   // close the producer gate
            // Do NOT reset s_try_idx here. This fires on both a real
            // connection drop and a failed page; resetting it on a failed
            // page would re-page slot 0 forever, never falling back to the
            // other bonded sinks. A successful connect resets the cursor to
            // 0 above, which restarts the next campaign from the most-recent
            // sink after a real drop.
            publish(BT_EV_DISCONNECTED, s_peer_bda);
        }
        break;

    case ESP_A2D_AUDIO_STATE_EVT:
        ESP_LOGI(TAG, "audio_state: %d", a2d->audio_stat.state);
        if (a2d->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
            publish(BT_EV_AUDIO_STARTED, s_peer_bda);
        } else {
            publish(BT_EV_AUDIO_SUSPENDED, s_peer_bda);
        }
        break;

    case ESP_A2D_MEDIA_CTRL_ACK_EVT: {
        uint8_t cmd = a2d->media_ctrl_stat.cmd;
        uint8_t st  = a2d->media_ctrl_stat.status;
        if (cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY &&
            st == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS &&
            s_media_state == MEDIA_STARTING) {
            ESP_LOGI(TAG, "src ready, starting media");
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
        } else if (cmd == ESP_A2D_MEDIA_CTRL_START) {
            if (st == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                ESP_LOGI(TAG, "*** media STARTED ***");
                s_media_state = MEDIA_STARTED;
                // Open the producer gate now that the encoder is pulling, so the
                // stream buffer fills from empty in step with the consumer instead
                // of pre-filling to full during the connect handshake.
                audio_pipeline_set_bt_streaming(true);
            } else {
                ESP_LOGW(TAG, "media start NACK (%d); retry next call", st);
                s_media_state = MEDIA_IDLE;
                audio_pipeline_set_bt_streaming(false);
            }
        } else if (cmd == ESP_A2D_MEDIA_CTRL_SUSPEND) {
            ESP_LOGI(TAG, "media SUSPENDED (status=%d)", st);
            s_media_state = MEDIA_IDLE;
            audio_pipeline_set_bt_streaming(false);
        }
        break;
    }

    case ESP_A2D_REPORT_SNK_CODEC_CAPS_EVT: {
        esp_a2d_mcc_t *caps = &a2d->a2d_report_snk_codec_caps_stat.mcc;
        if (caps->type == ESP_A2D_MCT_SBC) {
            ESP_LOGI(TAG, "sink SBC caps: sf=0x%x ch=0x%x bl=0x%x sb=0x%x "
                          "am=0x%x bp=%d-%d",
                     caps->cie.sbc_info.samp_freq,
                     caps->cie.sbc_info.ch_mode,
                     caps->cie.sbc_info.block_len,
                     caps->cie.sbc_info.num_subbands,
                     caps->cie.sbc_info.alloc_mthd,
                     caps->cie.sbc_info.min_bitpool,
                     caps->cie.sbc_info.max_bitpool);
        }
        break;
    }

    default:
        break;
    }
}

static void a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    bt_work_dispatch(av_event_hdlr, event, param, sizeof(esp_a2d_cb_param_t));
}

// ---- public API: connection control ---------------------------------------

esp_err_t bt_a2d_connect_bonded(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;   // BT skipped this boot (low battery)
    // Guard against re-entering connect while a previous page is still in
    // flight. Bluedroid can panic with StoreProhibited if
    // esp_a2d_source_connect() is called back-to-back without the prior
    // attempt completing, and the reconnect tick can fire while a prior
    // page is still running.
    if (s_link_state == LINK_PAGING || s_link_state == LINK_CONNECTED) {
        return ESP_OK;
    }
    if (s_radio_off) return ESP_OK;   // radio silenced for a bench measurement

    int n = esp_bt_gap_get_bond_device_num();
    if (n <= 0) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_bd_addr_t bonded[MAX_BONDED];
    int slots = (n > MAX_BONDED) ? MAX_BONDED : n;
    if (esp_bt_gap_get_bond_device_list(&slots, bonded) != ESP_OK || slots <= 0) {
        return ESP_ERR_NOT_FOUND;
    }

    // Build the candidate order: most-recently-connected sink first (if it's
    // known and still bonded), then the remaining bonded devices in list
    // order. s_try_idx walks this order across reconnect ticks.
    esp_bd_addr_t order[MAX_BONDED];
    int count = 0;
    esp_bd_addr_t last;
    int last_slot = -1;
    if (load_last_bda(last)) {
        for (int i = 0; i < slots; i++) {
            if (memcmp(bonded[i], last, ESP_BD_ADDR_LEN) == 0) {
                last_slot = i;
                break;
            }
        }
    }
    if (last_slot >= 0) {
        memcpy(order[count++], bonded[last_slot], ESP_BD_ADDR_LEN);
    }
    for (int i = 0; i < slots; i++) {
        if (i == last_slot) continue;
        memcpy(order[count++], bonded[i], ESP_BD_ADDR_LEN);
    }

    if (s_try_idx >= count) s_try_idx = 0;
    int idx = s_try_idx;
    s_try_idx = (s_try_idx + 1) % count;

    memcpy(s_peer_bda, order[idx], ESP_BD_ADDR_LEN);
    char addr[18];
    bda_str(s_peer_bda, addr);
    ESP_LOGI(TAG, "*** paging bonded peer %s (%d/%d) ***", addr, idx + 1, count);
    s_link_state = LINK_PAGING;
    return esp_a2d_source_connect(s_peer_bda);
}

void bt_a2d_reset_fail_blacklist(void)
{
    s_fail_bl_n = 0;
}

esp_err_t bt_a2d_start_pairing(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;   // BT skipped this boot (low battery)
    if (s_radio_off) return ESP_OK;   // radio silenced for a bench measurement
    ESP_LOGI(TAG, "*** entering pairing mode (inquiry) ***");
    memset(s_peer_bda, 0, sizeof(s_peer_bda));
    s_pairing_mode = true;
    s_link_state   = LINK_DISCOVERING;
    return esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY,
                                      CONFIG_GBHIFI_INQUIRY_DURATION, 0);
}

// Gate BT Classic TX for a clean bench noise measurement. `off` cancels any
// in-flight inquiry and blocks further inquiry/paging (see the guards in
// bt_a2d_start_pairing / bt_a2d_connect_bonded); `on` re-allows them.
void bt_a2d_set_radio(bool on)
{
    s_radio_off = !on;
    if (!on) {
        esp_bt_gap_cancel_discovery();
        s_pairing_mode = false;
    }
    ESP_LOGI(TAG, "BT radio %s", on ? "on" : "off (inquiry + paging gated)");
}

bool bt_a2d_has_bond(void)
{
    if (!s_inited) return false;   // BT skipped this boot (low battery)
    return esp_bt_gap_get_bond_device_num() > 0;
}

esp_err_t bt_a2d_clear_bonds(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;   // BT skipped this boot (low battery)
    // Remove every bond. The list API caps at MAX_BONDED per call, so loop
    // and re-query until the controller reports none left (bounded so a
    // misbehaving stack can't spin forever).
    for (int pass = 0; pass < 16; pass++) {
        int n = esp_bt_gap_get_bond_device_num();
        if (n <= 0) break;
        esp_bd_addr_t bonded[MAX_BONDED];
        int slots = (n > MAX_BONDED) ? MAX_BONDED : n;
        if (esp_bt_gap_get_bond_device_list(&slots, bonded) != ESP_OK) break;
        for (int i = 0; i < slots; i++) {
            char addr[18];
            bda_str(bonded[i], addr);
            ESP_LOGI(TAG, "removing bond %s", addr);
            esp_bt_gap_remove_bond_device(bonded[i]);
        }
    }

    // Drop the most-recently-connected record.
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY_LAST);
        nvs_commit(h);
        nvs_close(h);
    }
    s_try_idx = 0;
    ESP_LOGI(TAG, "all bonds + last-BDA record cleared");
    return ESP_OK;
}

esp_err_t bt_a2d_disconnect(void)
{
    if (!s_inited) return ESP_OK;   // BT skipped this boot: nothing to tear down
    if (s_link_state == LINK_DISCOVERING) {
        // Abort the inquiry AND neutralize the trailing DISCOVERY_STOPPED
        // callback: clear the discovering state so gap_cb won't page a sink that
        // was matched mid-inquiry (that page would fire an A2DP connect after
        // we've already left PAIRING, e.g. when the user bails to Mode A).
        esp_bt_gap_cancel_discovery();
        s_pairing_mode = false;
        s_link_state   = LINK_IDLE;
    }
    if (s_link_state == LINK_CONNECTED || s_link_state == LINK_PAGING) {
        esp_a2d_source_disconnect(s_peer_bda);
    }
    return ESP_OK;
}

bool bt_a2d_link_active(void)
{
    // True while any Bluetooth link activity is outstanding: an A2DP link/page
    // OR a still-up baseband ACL link. The ACL check is the one that matters for
    // Mode A: bt_a2d_disconnect() only starts the teardown, and its completion
    // runs on the BT work-task (which can't run while the CPU is light-sleeping),
    // so Mode A must stay awake until this goes false before its first sleep.
    if (!s_inited) return false;
    return s_acl_up ||
           s_link_state == LINK_CONNECTED || s_link_state == LINK_PAGING;
}

esp_err_t bt_a2d_media_start(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;   // BT skipped this boot (low battery)
    if (s_link_state != LINK_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_media_state == MEDIA_STARTED || s_media_state == MEDIA_STARTING) {
        return ESP_OK;
    }
    s_media_state = MEDIA_STARTING;
    ESP_LOGI(TAG, "media: checking source ready");
    return esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
}

esp_err_t bt_a2d_media_suspend(void)
{
    if (!s_inited) return ESP_OK;   // BT skipped this boot: nothing streaming
    if (s_media_state != MEDIA_STARTED) {
        return ESP_OK;
    }
    s_media_state = MEDIA_SUSPENDING;
    ESP_LOGI(TAG, "media: suspending");
    return esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
}

// ---- AVRCP target ---------------------------------------------------------
// We're an A2DP SOURCE; the sink acts as an AVRCP controller and opens an
// AVCTP channel (L2CAP PSM 0x17) to send transport/volume commands. Without
// AVRCP registered, L2CAP rejects that as an "unknown PSM" and the sink
// retries forever, flooding the log. Bringing up AVRCP target registers the
// PSM so the connection is accepted. We don't expose media metadata or act
// on the commands (the GBA has no transport to control); this just makes the
// sink stop hammering.
static void avrc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_TG_CONNECTION_STATE_EVT:
        ESP_LOGI(TAG, "AVRCP TG %s",
                 param->conn_stat.connected ? "connected" : "disconnected");
        break;
    default:
        break;   // passthrough / volume commands acked by the stack, ignored here
    }
}

// ---- public init ----------------------------------------------------------

esp_err_t bt_a2d_init(void)
{
    if (s_inited) return ESP_OK;

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

#if CONFIG_BT_BLE_ENABLED
    // Dual mode so the BLE GATT config server can run beside the A2DP source.
    // Keep the BLE controller memory (do NOT mem-release it) and bring the
    // controller up in BTDM. sdkconfig must select CONFIG_BTDM_CTRL_MODE_BTDM
    // to match.
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BTDM));
#else
    // Classic-only build: reclaim the unused BLE controller RAM, then bring up
    // the controller in BR/EDR-only mode (CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY).
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
#endif

    // Cap BR/EDR TX at -3 dBm from the first RF activity. The controller
    // defaults to +9 dBm, so without this the boot-time inquiry/page bursts
    // run at full power -- enough extra load to brown out the boost converter
    // on a low battery. -3 dBm is plenty for a headphone within a meter; the
    // CONNECTED handler re-asserts the same level per link.
    esp_err_t pe = esp_bredr_tx_power_set(ESP_PWR_LVL_N3, ESP_PWR_LVL_N3);
    if (pe != ESP_OK) {
        ESP_LOGW(TAG, "boot tx_power_set: %s", esp_err_to_name(pe));
    }

    // Controller-level modem sleep. sdkconfig pins this to
    // MODEM_SLEEP_MODE_ORIG with the main XTAL as the low-power clock.
    // Pairs with sniff-mode negotiation initiated by the sink so the
    // controller can gate its RF blocks between sniff intervals.
    esp_err_t sleep_err = esp_bt_sleep_enable();
    if (sleep_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_bt_sleep_enable: %s", esp_err_to_name(sleep_err));
    }

    esp_bluedroid_config_t bd_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&bd_cfg));
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    // SSP: I/O capability = NO_INPUT_NO_OUTPUT for Just Works pairing,
    // no user interaction needed. Bluedroid persists link keys in NVS.
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));

    s_work_q = xQueueCreate(10, sizeof(bt_work_msg_t));
    xTaskCreate(bt_work_task, "bt_work", 4096, NULL, 10, &s_work_th);

    esp_bt_gap_set_device_name(LOCAL_DEVICE_NAME);
    esp_bt_gap_register_callback(gap_cb);

    // AVRCP target: registers the AVCTP PSM so the sink's AVRCP-controller
    // connection is accepted instead of rejected+retried (log-spam fix).
    // Bluedroid requires this BEFORE A2DP init ("AVRC Target is expected to be
    // initialized in advance of A2DP") so the SDP/PSM records are set up first.
    esp_avrc_tg_init();
    esp_avrc_tg_register_callback(avrc_tg_cb);

    esp_a2d_source_init();
    esp_a2d_register_callback(a2d_cb);
    esp_a2d_source_register_data_callback(a2d_data_cb);

    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

    char addr[18];
    bda_str(esp_bt_dev_get_address(), addr);
    ESP_LOGI(TAG, "own BDA: %s, name: %s", addr, LOCAL_DEVICE_NAME);

    s_inited = true;
    return ESP_OK;
}
