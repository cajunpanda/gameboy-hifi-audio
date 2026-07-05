#include "settings.h"

#include "sdkconfig.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "settings";

// A `default n` Kconfig bool is left undefined by the config generator (not
// defined as 0), so referencing it directly in an initializer fails to compile.
// Guard each default-off symbol so the initializer below sees a value.
#ifndef CONFIG_GBHIFI_DSP_BT_EQ_DEFAULT_ON
#define CONFIG_GBHIFI_DSP_BT_EQ_DEFAULT_ON 0
#endif
#ifndef CONFIG_GBHIFI_DSP_HP_EQ_DEFAULT_ON
#define CONFIG_GBHIFI_DSP_HP_EQ_DEFAULT_ON 0
#endif
#ifndef CONFIG_GBHIFI_DEFAULT_MODE_A
#define CONFIG_GBHIFI_DEFAULT_MODE_A 0
#endif
#ifndef CONFIG_GBHIFI_UNPLUG_TO_B
#define CONFIG_GBHIFI_UNPLUG_TO_B 0
#endif
#ifndef CONFIG_GBHIFI_AUTO_CONNECT
#define CONFIG_GBHIFI_AUTO_CONNECT 0
#endif

#define NVS_NAMESPACE "gbhifi_cfg"
#define NVS_KEY       "dsp"

// Clamp helpers: every external value is sanitised before it reaches the DSP.
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
// Volume is unsigned (0..100), so only the upper bound needs clamping;
// clamping >= 0 would trip -Werror=type-limits.
#define CLAMP_VOL(v) ((v) > 100 ? 100 : (v))
#define EQ_DB_MIN  (-12)
#define EQ_DB_MAX  (12)
#define SFX_DB_MIN (-24)
#define SFX_DB_MAX (6)
// Hold-menu thresholds (ms): sane bounds for a deliberate, gameplay-safe gesture.
#define HOLD_MS_MIN  (500)
#define HOLD_MS_MAX  (15000)
#define NR_HZ_MAX    (21000)   // noise-reduction filter freq ceiling (< Nyquist)
#define NR_Q_MIN     (1)
#define NR_Q_MAX     (40)
#define GATE_DB_MIN     (-90)   // gate threshold floor, dBFS
#define GATE_RANGE_MAX  (60)    // max gate attenuation depth, dB

static SemaphoreHandle_t s_lock;
static gbhifi_settings_t  s_cur;
static volatile uint32_t s_generation;

static const gbhifi_settings_t s_defaults = {
    .version         = GBHIFI_SETTINGS_VERSION,
    .speaker_vol_pct = CONFIG_GBHIFI_DSP_DEFAULT_VOLUME,
    .bt_vol_pct      = CONFIG_GBHIFI_DSP_BT_DEFAULT_VOLUME,
    .eq_enabled      = CONFIG_GBHIFI_DSP_EQ_DEFAULT_ON,
    .eq_bass_db      = CONFIG_GBHIFI_DSP_EQ_BASS_DB,
    .eq_mid_db       = CONFIG_GBHIFI_DSP_EQ_MID_DB,
    .eq_treble_db    = CONFIG_GBHIFI_DSP_EQ_TREBLE_DB,
    .eq_hp_enabled   = CONFIG_GBHIFI_DSP_HP_EQ_DEFAULT_ON,
    .eq_hp_bass_db   = CONFIG_GBHIFI_DSP_HP_EQ_BASS_DB,
    .eq_hp_mid_db    = CONFIG_GBHIFI_DSP_HP_EQ_MID_DB,
    .eq_hp_treble_db = CONFIG_GBHIFI_DSP_HP_EQ_TREBLE_DB,
    .eq_bt_enabled   = CONFIG_GBHIFI_DSP_BT_EQ_DEFAULT_ON,
    .eq_bt_bass_db   = CONFIG_GBHIFI_DSP_BT_EQ_BASS_DB,
    .eq_bt_mid_db    = CONFIG_GBHIFI_DSP_BT_EQ_MID_DB,
    .eq_bt_treble_db = CONFIG_GBHIFI_DSP_BT_EQ_TREBLE_DB,
    .sfx_enabled     = CONFIG_GBHIFI_DSP_SFX_DEFAULT_ON,
    .sfx_level_db    = CONFIG_GBHIFI_DSP_SFX_LEVEL_DB,
    .nr_hpf_hz       = CONFIG_GBHIFI_DSP_NR_HPF_HZ,
    .nr_lpf_hz       = CONFIG_GBHIFI_DSP_NR_LPF_HZ,
    .nr_notch_hz     = CONFIG_GBHIFI_DSP_NR_NOTCH_HZ,
    .nr_notch_q      = CONFIG_GBHIFI_DSP_NR_NOTCH_Q,
    .nr_gate_thresh_db = CONFIG_GBHIFI_DSP_NR_GATE_THRESH_DB,
    .nr_gate_range_db  = CONFIG_GBHIFI_DSP_NR_GATE_RANGE_DB,
    .mode_a          = CONFIG_GBHIFI_DEFAULT_MODE_A,
    .unplug_to_b     = CONFIG_GBHIFI_UNPLUG_TO_B,
    .auto_connect    = CONFIG_GBHIFI_AUTO_CONNECT,
    // Absolute hold thresholds: connect at CONNECT_HOLD_MS, pair + PAIR_EXTRA_MS
    // beyond that, mode + MODE_EXTRA_MS beyond pair.
    .hold_connect_ms   = CONFIG_GBHIFI_CONNECT_HOLD_MS,
    .hold_pair_ms      = CONFIG_GBHIFI_CONNECT_HOLD_MS + CONFIG_GBHIFI_PAIR_EXTRA_MS,
    .hold_mode_ms      = CONFIG_GBHIFI_CONNECT_HOLD_MS + CONFIG_GBHIFI_PAIR_EXTRA_MS
                         + CONFIG_GBHIFI_MODE_EXTRA_MS,
    .hold_mode_exit_ms = CONFIG_GBHIFI_MODE_EXIT_MS,
};

// Re-clamp every field. Defends against a corrupt/forward-version NVS blob and
// keeps the DSP from ever seeing an out-of-range coefficient input.
static void sanitise(gbhifi_settings_t *s)
{
    s->speaker_vol_pct = CLAMP_VOL(s->speaker_vol_pct);
    s->bt_vol_pct      = CLAMP_VOL(s->bt_vol_pct);
    s->eq_bass_db      = CLAMP(s->eq_bass_db,      EQ_DB_MIN,  EQ_DB_MAX);
    s->eq_mid_db       = CLAMP(s->eq_mid_db,       EQ_DB_MIN,  EQ_DB_MAX);
    s->eq_treble_db    = CLAMP(s->eq_treble_db,    EQ_DB_MIN,  EQ_DB_MAX);
    s->eq_hp_bass_db   = CLAMP(s->eq_hp_bass_db,   EQ_DB_MIN,  EQ_DB_MAX);
    s->eq_hp_mid_db    = CLAMP(s->eq_hp_mid_db,    EQ_DB_MIN,  EQ_DB_MAX);
    s->eq_hp_treble_db = CLAMP(s->eq_hp_treble_db, EQ_DB_MIN,  EQ_DB_MAX);
    s->eq_bt_bass_db   = CLAMP(s->eq_bt_bass_db,   EQ_DB_MIN,  EQ_DB_MAX);
    s->eq_bt_mid_db    = CLAMP(s->eq_bt_mid_db,    EQ_DB_MIN,  EQ_DB_MAX);
    s->eq_bt_treble_db = CLAMP(s->eq_bt_treble_db, EQ_DB_MIN,  EQ_DB_MAX);
    s->sfx_level_db    = CLAMP(s->sfx_level_db,    SFX_DB_MIN, SFX_DB_MAX);
    if (s->nr_hpf_hz   > NR_HZ_MAX) s->nr_hpf_hz   = NR_HZ_MAX;
    if (s->nr_lpf_hz   > NR_HZ_MAX) s->nr_lpf_hz   = NR_HZ_MAX;
    if (s->nr_notch_hz > NR_HZ_MAX) s->nr_notch_hz = NR_HZ_MAX;
    s->nr_notch_q      = CLAMP(s->nr_notch_q, NR_Q_MIN, NR_Q_MAX);
    s->nr_gate_thresh_db = CLAMP(s->nr_gate_thresh_db, GATE_DB_MIN, 0);
    if (s->nr_gate_range_db > GATE_RANGE_MAX) s->nr_gate_range_db = GATE_RANGE_MAX;

    // Hold thresholds: clamp each, then enforce strictly-increasing order so
    // buttons.c's per-rung re-arm intervals (pair-connect, mode-pair) stay
    // positive. mode_exit is independent.
    s->hold_connect_ms   = CLAMP(s->hold_connect_ms,   HOLD_MS_MIN, HOLD_MS_MAX);
    s->hold_pair_ms      = CLAMP(s->hold_pair_ms,      HOLD_MS_MIN, HOLD_MS_MAX);
    s->hold_mode_ms      = CLAMP(s->hold_mode_ms,      HOLD_MS_MIN, HOLD_MS_MAX);
    s->hold_mode_exit_ms = CLAMP(s->hold_mode_exit_ms, HOLD_MS_MIN, HOLD_MS_MAX);
    if (s->hold_pair_ms <= s->hold_connect_ms) s->hold_pair_ms = s->hold_connect_ms + 500;
    if (s->hold_mode_ms <= s->hold_pair_ms)    s->hold_mode_ms = s->hold_pair_ms + 500;
}

esp_err_t settings_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return ESP_ERR_NO_MEM;
    }

    s_cur = s_defaults;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        gbhifi_settings_t blob;
        size_t len = sizeof(blob);
        err = nvs_get_blob(h, NVS_KEY, &blob, &len);
        if (err == ESP_OK && len == sizeof(blob) &&
            blob.version == GBHIFI_SETTINGS_VERSION) {
            sanitise(&blob);
            s_cur = blob;
            ESP_LOGI(TAG, "loaded settings from NVS");
        } else {
            ESP_LOGI(TAG, "no usable NVS blob (err=%s, ver/len mismatch) - defaults",
                     esp_err_to_name(err));
        }
        nvs_close(h);
    } else {
        ESP_LOGI(TAG, "NVS namespace absent - defaults");
    }

    s_generation++;
    ESP_LOGI(TAG, "spkVol=%u%% btVol=%u%% spkEQ=%d[%d/%d/%d] hpEQ=%d[%d/%d/%d] btEQ=%d[%d/%d/%d] sfx=%d[%ddB]",
             s_cur.speaker_vol_pct, s_cur.bt_vol_pct, s_cur.eq_enabled,
             s_cur.eq_bass_db, s_cur.eq_mid_db, s_cur.eq_treble_db,
             s_cur.eq_hp_enabled,
             s_cur.eq_hp_bass_db, s_cur.eq_hp_mid_db, s_cur.eq_hp_treble_db,
             s_cur.eq_bt_enabled,
             s_cur.eq_bt_bass_db, s_cur.eq_bt_mid_db, s_cur.eq_bt_treble_db,
             s_cur.sfx_enabled, s_cur.sfx_level_db);
    ESP_LOGI(TAG, "mode=%c unplug_to_B=%d auto_connect=%d hold[con/pair/mode/exit]=%u/%u/%u/%u ms",
             s_cur.mode_a ? 'A' : 'B', s_cur.unplug_to_b, s_cur.auto_connect,
             s_cur.hold_connect_ms, s_cur.hold_pair_ms,
             s_cur.hold_mode_ms, s_cur.hold_mode_exit_ms);
    return ESP_OK;
}

void settings_get(gbhifi_settings_t *out)
{
    if (!out) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cur;
    xSemaphoreGive(s_lock);
}

uint32_t settings_generation(void)
{
    return s_generation;
}

esp_err_t settings_set_volume(uint8_t pct)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cur.speaker_vol_pct = CLAMP_VOL(pct);
    s_generation++;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t settings_set_eq(bool enabled, int8_t bass_db, int8_t mid_db, int8_t treble_db)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cur.eq_enabled   = enabled;
    s_cur.eq_bass_db   = CLAMP(bass_db,   EQ_DB_MIN, EQ_DB_MAX);
    s_cur.eq_mid_db    = CLAMP(mid_db,    EQ_DB_MIN, EQ_DB_MAX);
    s_cur.eq_treble_db = CLAMP(treble_db, EQ_DB_MIN, EQ_DB_MAX);
    s_generation++;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t settings_set_hp_eq(bool enabled, int8_t bass_db, int8_t mid_db, int8_t treble_db)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cur.eq_hp_enabled   = enabled;
    s_cur.eq_hp_bass_db   = CLAMP(bass_db,   EQ_DB_MIN, EQ_DB_MAX);
    s_cur.eq_hp_mid_db    = CLAMP(mid_db,    EQ_DB_MIN, EQ_DB_MAX);
    s_cur.eq_hp_treble_db = CLAMP(treble_db, EQ_DB_MIN, EQ_DB_MAX);
    s_generation++;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t settings_set_bt_volume(uint8_t pct)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cur.bt_vol_pct = CLAMP_VOL(pct);
    s_generation++;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t settings_set_bt_eq(bool enabled, int8_t bass_db, int8_t mid_db, int8_t treble_db)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cur.eq_bt_enabled   = enabled;
    s_cur.eq_bt_bass_db   = CLAMP(bass_db,   EQ_DB_MIN, EQ_DB_MAX);
    s_cur.eq_bt_mid_db    = CLAMP(mid_db,    EQ_DB_MIN, EQ_DB_MAX);
    s_cur.eq_bt_treble_db = CLAMP(treble_db, EQ_DB_MIN, EQ_DB_MAX);
    s_generation++;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t settings_set_sfx(bool enabled, int8_t level_db)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cur.sfx_enabled  = enabled;
    s_cur.sfx_level_db = CLAMP(level_db, SFX_DB_MIN, SFX_DB_MAX);
    s_generation++;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t settings_set_nr(uint16_t hpf_hz, uint16_t lpf_hz, uint16_t notch_hz,
                          uint8_t notch_q)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cur.nr_hpf_hz   = (hpf_hz   > NR_HZ_MAX) ? NR_HZ_MAX : hpf_hz;
    s_cur.nr_lpf_hz   = (lpf_hz   > NR_HZ_MAX) ? NR_HZ_MAX : lpf_hz;
    s_cur.nr_notch_hz = (notch_hz > NR_HZ_MAX) ? NR_HZ_MAX : notch_hz;
    s_cur.nr_notch_q  = CLAMP(notch_q, NR_Q_MIN, NR_Q_MAX);
    s_generation++;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t settings_set_nr_gate(int8_t thresh_db, uint8_t range_db)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cur.nr_gate_thresh_db = CLAMP(thresh_db, GATE_DB_MIN, 0);
    s_cur.nr_gate_range_db  = (range_db > GATE_RANGE_MAX) ? GATE_RANGE_MAX : range_db;
    s_generation++;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t settings_set_mode_a(bool mode_a)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cur.mode_a = mode_a;
    s_generation++;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t settings_set_unplug_to_b(bool unplug_to_b)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cur.unplug_to_b = unplug_to_b;
    s_generation++;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t settings_set_auto_connect(bool auto_connect)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cur.auto_connect = auto_connect;
    s_generation++;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t settings_set_hold_timings(uint16_t connect_ms, uint16_t pair_ms,
                                    uint16_t mode_ms, uint16_t mode_exit_ms)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cur.hold_connect_ms   = connect_ms;
    s_cur.hold_pair_ms      = pair_ms;
    s_cur.hold_mode_ms      = mode_ms;
    s_cur.hold_mode_exit_ms = mode_exit_ms;
    sanitise(&s_cur);   // clamp + re-order the just-set thresholds
    s_generation++;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t settings_commit(void)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    gbhifi_settings_t snap;
    settings_get(&snap);
    snap.version = GBHIFI_SETTINGS_VERSION;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, NVS_KEY, &snap, sizeof(snap));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "commit %s", esp_err_to_name(err));
    return err;
}

