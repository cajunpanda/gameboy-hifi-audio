#include "app_sm.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "audio_pipeline.h"
#include "ble_config.h"
#include "bt_a2d.h"
#include "buttons.h"
#include "dsp.h"
#include "es8388.h"
#include "pinmap.h"
#include "settings.h"

#include "sdkconfig.h"
#include "sfx.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
// Fire a synth cue on the local speaker path.
#define CUE(id) sfx_trigger_synth(id)

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_serial_output.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"

static const char *TAG = "app_sm";

// Mode A duty-cycle: light-sleep wake cadence to poll the VOL wheel. ~200 ms
// gives imperceptible volume lag while the CPU sleeps >98% of the time.
// ~3 ms of work per wake = ~1.5% duty.
#define MODE_A_POLL_MS  200

// ---- states ---------------------------------------------------------------

typedef enum {
    ST_STANDBY,
    ST_PAIRING,
    ST_CONNECTED_IDLE,
    ST_STREAMING,
    ST_DEEP_IDLE,
    // BT radio idle (modem-sleep), ESP32 stays awake carrying GBA audio out the
    // digital speaker path. The standby/pairing give-up target: deep sleep would
    // silence the digital speaker (it runs through the chip) and there's no audio
    // wake source. Terminal until a Connect/Pair hold or an HP plug.
    ST_LOCAL_ONLY,
    // Mode A (analog bypass / battery saver): the ES8388 carries LIN2/RIN2 to its
    // outputs in hardware with the digital path stopped (pipeline parked, I2S/MCLK
    // off), and the ESP32 CPU runs a duty-cycled light-sleep loop (~200 ms wake to
    // poll VOL wheel, then re-sleep). The BT controller is left up (a manual
    // esp_light_sleep_start() sleeps the CPU even with the controller enabled).
    // ~36 mA vs ~60 mA Mode B. Wakes: timer (wheel), ext0 (HP edge to reroute),
    // ext1 (R hold to exit to Mode B). mode_a_run() owns the whole lifecycle; the
    // normal event loop doesn't dispatch here.
    ST_MODE_A,
    // OTA (BLE firmware update). Entered when the BLE config server reports a
    // client wants to flash: drop A2DP, stop the audio pipeline, and mute the
    // speaker amp so the multi-hundred-ms flash erase + chunk writes (cache
    // disabled) can't garble live audio, then let ble_config drive the transfer.
    // A clean finish reboots into the new image; an abort/error/disconnect returns
    // to STANDBY with the pipeline resumed.
    ST_OTA,
} app_state_t;

static const char *state_name(app_state_t s)
{
    switch (s) {
    case ST_STANDBY:        return "STANDBY";
    case ST_PAIRING:        return "PAIRING";
    case ST_CONNECTED_IDLE: return "CONNECTED_IDLE";
    case ST_STREAMING:      return "STREAMING";
    case ST_DEEP_IDLE:      return "DEEP_IDLE";
    case ST_LOCAL_ONLY:     return "LOCAL_ONLY";
    case ST_MODE_A:         return "MODE_A";
    case ST_OTA:            return "OTA";
    }
    return "?";
}

// ---- event queue ----------------------------------------------------------
// All inputs to the state machine (button presses, BT-layer events, audio
// silence transitions, internal timer ticks) fold into a single queue drained by
// the SM task, so every transition runs in one context with no cross-task races
// on s_state.

typedef enum {
    EV_BTN_CP_SHORT,        // Connect/Pair short press (benign; wakes DEEP_IDLE)
    EV_BTN_CP_CONNECT,      // released in the connect zone
    EV_BTN_CP_PAIR,         // released in the pair zone
    EV_BTN_CP_MODE,         // released in the mode-toggle zone: toggle Mode A/B
    EV_BTN_CP_ZONE_CONNECT, // hold crossed the connect rung (feedback chime)
    EV_BTN_CP_ZONE_PAIR,    // hold crossed the pair rung (feedback chime)
    EV_BTN_CP_ZONE_MODE,    // hold crossed the mode rung (feedback chime)
    EV_BTN_HP_PLUG,
    EV_BTN_HP_UNPLUG,
    EV_BT_CONNECTED,
    EV_BT_DISCONNECTED,
    EV_BT_AUDIO_STARTED,
    EV_BT_AUDIO_SUSPENDED,
    EV_BT_PAIRING_TIMEOUT,
    EV_AUDIO_ACTIVE,        // peak rose above silence threshold
    EV_AUDIO_SILENT,        // 2 s of below-threshold peaks
    EV_STANDBY_TIMEOUT,     // STANDBY give-up elapsed (CONFIG_GBHIFI_STANDBY_TIMEOUT_S)
    EV_RECONNECT_TICK,      // reconnect retry tick (CONFIG_GBHIFI_RECONNECT_TICK_S)
    EV_FORCE_SLEEP,         // force DEEP_IDLE (console `sleep` wake-reliability test only)
    EV_PAIRING_SESSION_TIMEOUT, // pairing-mode session elapsed (CONFIG_GBHIFI_PAIRING_TIMEOUT_S)
    EV_MODE_CHANGE,         // desired (settings.mode_a) != active mode, transition
    EV_OTA_BEGIN,           // BLE client wants to flash: quiesce + enter ST_OTA
    EV_OTA_END,             // OTA ended without reboot (abort/error/disconnect): resume
    EV_GATE_CHANGED,        // noise-gate deep-silence state changed (drives amp mute)
} sm_event_t;

#define EVQ_DEPTH 16

static QueueHandle_t s_evq          = NULL;
static TimerHandle_t s_standby_to_t = NULL;  // 30 s STANDBY give-up timer
static TimerHandle_t s_reconnect_t  = NULL;  // 5 s STANDBY reconnect retry
static TimerHandle_t s_pairing_to_t = NULL;  // pairing-mode session give-up
// Boot-mode decision handed down from main.c (settings + reset reason + CP-hold);
// true means drop straight into Mode A at boot (main.c also skipped BT init for it).
static bool          s_boot_into_mode_a = false;

static app_state_t s_state = ST_STANDBY;

// One-shot: suppress the arrival cue (CONNECT / PAIRING) for the very first
// connect or pairing after power-on, so the mod doesn't beep over the Game Boy
// boot chime. Consumed at that first arrival; later reconnects (e.g. after a
// mid-session sink drop) cue normally.
static bool s_boot_arrival = true;

// Active codec path. Mirrors the ES8388 mode the hardware is actually in.
// es8388_init() leaves it in DSP, so we boot DSP and the mode-change poll drives
// it toward the sticky settings.mode_a preference.
static es8388_mode_t s_active_mode = ES8388_MODE_DSP;

// VOL wheel (GPIO39 = ADC1_CHANNEL_3). Handle is NULL if the ADC failed to init.
// s_wheel_enabled gates whether the reading is applied to volume. ON by default:
// the wheel is the GBA's physical volume control. `wheel off` disables it when the
// console `vol` should win (or the wheel isn't wired). s_vol_last_pct is the last
// applied value (hysteresis).
#define VOL_ADC_CHANNEL  ADC_CHANNEL_3
// Hysteresis band (%) and an EMA low-pass on the raw ADC, both to tame the VOL
// line's jitter. Without it the reading bounces ~2-3% every poll, churning the
// volume into a flood of DSP coeff recomputes (Mode B) / I2C writes (Mode A).
// EMA jitter rejection depends only on alpha (not the rate); at the dedicated
// task's VOL_POLL_MS cadence, alpha=0.30 gives a ~0.25 s settle (time constant
// poll/alpha) -- jitter shrinks below the hysteresis band while a real wheel turn
// tracks in a quarter second. (Mode A samples the same EMA at its own 200 ms
// light-sleep cadence, so it settles a touch slower; fine for the analog path.)
#define VOL_HYSTERESIS_PCT 3
#define VOL_EMA_ALPHA      0.30f
// VOL wheel calibration. The wheel divider is VCC-referenced, so the wiper never
// reaches 0 or full-scale: min ~ raw 330, max ~ raw 3600, noisy on the high-Z VOL
// line. Map the usable window to 0..100 with a small dead-band at each end so the
// wheel reliably bottoms at 0% and tops at 100%, and oversample each read to tame
// the jitter.
#define VOL_RAW_MIN     400
#define VOL_RAW_MAX     3550
#define VOL_OVERSAMPLE  16
// Mode B wheel-poll cadence for the dedicated vol_task (see vol_task). Faster and
// far more regular than the old 200 ms software-timer poll, which rode the
// priority-1 timer daemon and got starved to hundreds of ms during A2DP streaming
// (both cores saturated), making the wheel feel laggy. 80 ms tracks a wheel turn
// promptly at negligible core-1 cost.
#define VOL_POLL_MS     80

// Battery sense: VBAT (the U3 boost input) through an R20/R21 = 100k/100k divider
// into ADC1_CH0, so VBAT_mV = 2 * sense_mV. Shares the ADC1 oneshot unit with the
// wheel and needs adc_cali to turn raw counts into millivolts.
#define VBAT_ADC_CHANNEL  ADC_CHANNEL_0     // GPIO36 (PIN_VBAT_ADC)
#define VBAT_DIVIDER_NUM  2                 // (R20 + R21) / R21 = 200k / 100k
#define VBAT_OVERSAMPLE   16
// The GBA volume pot is powered from VBAT, so the wiper reading scales with the
// battery. Normalising each raw reading back to VBAT_REF_MV (the VBAT the
// VOL_RAW_MIN/MAX window was calibrated at) keeps that calibration valid as the
// battery droops.
#define VBAT_REF_MV       3200
// Low-battery latch: warn under VBAT_LOW_MV, clear only above +HYST (2x AA GBA
// rail: ~3.2 V full, ~2.4 V flat). Rough %: empty..full window below.
#define VBAT_LOW_MV       2500
#define VBAT_LOW_HYST_MV  150
#define VBAT_EMPTY_MV     2400
#define VBAT_FULL_MV      3200

static adc_oneshot_unit_handle_t s_vol_adc = NULL;
static adc_cali_handle_t s_adc1_cali = NULL;   // ADC1 raw->mV (NULL = no battery mV)
static bool  s_batt_low      = false;   // low-battery latch (hysteresis)
static bool  s_wheel_enabled = true;
static int   s_vol_last_pct  = -1;
static float s_wheel_ema     = -1.0f;   // EMA of the raw ADC (<0 = unseeded)

// Tracks whether wired headphones are plugged into the GBA's jack (the HP-detect
// line is HIGH). While true, the A2DP stream is suspended so the wired path
// carries audio with zero latency (hot-swap). The flag is updated centrally at the
// top of the SM event loop and consulted in the state handlers + CI entry prime.
static bool s_hp_plugged = false;

// Debug/bench HP-detect override. -1 = follow the real GPIO (normal); 0 = forced
// UNPLUGGED; 1 = forced PLUGGED. Driven by the `hp` console command so a meter
// reading doesn't require physically holding the jack. While forced, real jack
// edges are ignored (see on_button).
static int s_hp_force = -1;

// Noise-gate amp mute: set by the DSP (on_gate_mute) when the gate is deep in
// silence, so pam_apply also drops the speaker amp to kill its Class-D idle hiss.
static volatile bool s_gate_muting = false;

static void post(sm_event_t ev)
{
    uint8_t e = (uint8_t)ev;
    xQueueSend(s_evq, &e, 0);
}

// ---- speaker amp ----------------------------------------------------------
// The state machine owns whether the speaker plays the GBA's local analog feed.
// Speaker on in every state except STREAMING: when BT is carrying audio, the
// local speaker would echo the same content half a second late.

static void pam_set_unmuted(bool unmuted)
{
    gpio_set_level(PIN_PAM_SD, unmuted ? 1 : 0);
}

// Single rule for the local-speaker amp: it carries the GBA's audio unless
// another path is active. Muted when BT is STREAMING (the sink carries the audio)
// or when wired headphones are plugged in (the ES8388 HP amp drives them, so the
// external speaker must not echo it). PAIRING is deliberately not muted so the
// pairing chime + game audio play while the user sets up headphones.
static void pam_apply(void)
{
    bool unmuted = (s_state != ST_STREAMING) && !s_hp_plugged && !s_gate_muting;
    pam_set_unmuted(unmuted);
}

static void pam_init(void)
{
    // After a deep-sleep wake, PIN_PAM_SD is still latched by the RTC
    // hold we enabled before sleeping. Release that hold first or the
    // gpio_config + set_level calls below silently no-op against the
    // latched value. Harmless on a cold boot (nothing was held).
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(PIN_PAM_SD);

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_PAM_SD,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    pam_set_unmuted(true);
}

// ---- VOL wheel (GPIO39 ADC1) ----------------------------------------------
// The GBA volume wheel is a pot whose wiper feeds GPIO39 (input-only, ADC1). In
// Mode B it drives the local-speaker DSP volume; in Mode A it drives the ES8388
// output drivers over I2C. Read on the ~200 ms poll, applied only on a change of
// at least the hysteresis band so it doesn't churn settings / spam the I2C bus.

static void vol_adc_init(void)
{
    if (s_vol_adc) return;   // already primed early (app_sm_prime_volume); ADC1
                             // unit can only be created once, re-creating errors.
    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&unit_cfg, &s_vol_adc) != ESP_OK) {
        ESP_LOGW(TAG, "VOL ADC init failed; wheel disabled");
        s_vol_adc = NULL;
        return;
    }
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,        // full ~0..3.1 V span
        .bitwidth = ADC_BITWIDTH_DEFAULT,   // 12-bit on ESP32
    };
    if (adc_oneshot_config_channel(s_vol_adc, VOL_ADC_CHANNEL, &chan_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "VOL ADC channel cfg failed; wheel disabled");
        s_vol_adc = NULL;
        return;
    }

    // Battery sense shares the ADC1 unit. The cali handle turns raw counts into
    // millivolts; without it VBAT and the battery-referenced wheel fall back to
    // raw-only behaviour, so a failure here is non-fatal.
    if (adc_oneshot_config_channel(s_vol_adc, VBAT_ADC_CHANNEL, &chan_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "VBAT ADC channel cfg failed; battery sense off");
    }
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id  = ADC_UNIT_1,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_line_fitting(&cali_cfg, &s_adc1_cali) != ESP_OK) {
        ESP_LOGW(TAG, "ADC1 cali init failed; battery mV unavailable");
        s_adc1_cali = NULL;
    }
}

// Oversampled raw ADC read (mean of VOL_OVERSAMPLE samples), or -1 if the ADC is
// unavailable. Averaging cuts the high-Z VOL line's broadband noise (~200 counts
// raw, ~50 after 16x) below the hysteresis band.
static int vol_read_raw(void)
{
    if (!s_vol_adc) return -1;
    int acc = 0;
    for (int i = 0; i < VOL_OVERSAMPLE; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_vol_adc, VOL_ADC_CHANNEL, &raw) != ESP_OK) return -1;
        acc += raw;
    }
    return acc / VOL_OVERSAMPLE;
}

// Map a raw ADC reading to 0..100%, calibrated to the VCC-referenced wheel's
// usable [VOL_RAW_MIN, VOL_RAW_MAX] window with a dead-band at each end.
static int raw_to_pct(int raw)
{
    if (raw < 0)            return -1;
    if (raw <= VOL_RAW_MIN) return 0;
    if (raw >= VOL_RAW_MAX) return 100;
    return (raw - VOL_RAW_MIN) * 100 / (VOL_RAW_MAX - VOL_RAW_MIN);
}

// Battery-normalise a raw wheel reading: the pot is powered from VBAT, so the
// wiper (and thus the raw count) scales with the battery. Rescale to VBAT_REF_MV
// so raw_to_pct's fixed window holds as VBAT droops. No-op when the battery
// reading is unavailable (falls back to the plain raw value).
static int vol_batt_correct(int raw)
{
    if (raw < 0) return raw;
    int vbat = app_sm_read_vbat_mv();
    if (vbat <= 0) return raw;
    long c = (long)raw * VBAT_REF_MV / vbat;
    return (c > 4095) ? 4095 : (int)c;
}

// Read the wheel as 0..100%, or -1 if the ADC is unavailable. Unsmoothed: used by
// the console `wheel` read-out so it shows the live ADC value.
static int vol_read_pct(void)
{
    return raw_to_pct(vol_batt_correct(vol_read_raw()));
}

// EMA-smoothed wheel read (0..100%), or -1 if the ADC is unavailable. Both the
// Mode B poll and the Mode A duty loop apply volume from this so the jitter
// doesn't churn the volume. Single EMA state shared by both (they never run
// concurrently: mode_a_run() owns the CPU while in Mode A).
static int vol_read_pct_smoothed(void)
{
    int raw = vol_read_raw();
    if (raw < 0) return -1;
    if (s_wheel_ema < 0.0f) s_wheel_ema = (float)raw;          // seed on first read
    else s_wheel_ema += ((float)raw - s_wheel_ema) * VOL_EMA_ALPHA;
    return raw_to_pct(vol_batt_correct((int)(s_wheel_ema + 0.5f)));
}

void app_sm_vol_wheel_read(int *raw_out, int *pct_out)
{
    int raw = vol_read_raw();
    if (raw_out) *raw_out = raw;                          // live ADC (uncorrected)
    if (pct_out) *pct_out = raw_to_pct(vol_batt_correct(raw));
}

// Read VBAT in millivolts, or -1 if the battery sense/cali is unavailable.
// Oversamples the divided sense node, converts to mV via the ADC1 calibration,
// then undoes the R20/R21 divider.
int app_sm_read_vbat_mv(void)
{
    if (!s_vol_adc || !s_adc1_cali) return -1;
    int acc = 0;
    for (int i = 0; i < VBAT_OVERSAMPLE; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_vol_adc, VBAT_ADC_CHANNEL, &raw) != ESP_OK) return -1;
        acc += raw;
    }
    int sense_mv = 0;
    if (adc_cali_raw_to_voltage(s_adc1_cali, acc / VBAT_OVERSAMPLE, &sense_mv) != ESP_OK) {
        return -1;
    }
    return sense_mv * VBAT_DIVIDER_NUM;
}

// Rough battery percentage from a VBAT reading, linear across the 2x AA window.
// -1 if the reading is invalid.
int app_sm_batt_pct(int vbat_mv)
{
    if (vbat_mv < 0)              return -1;
    if (vbat_mv <= VBAT_EMPTY_MV) return 0;
    if (vbat_mv >= VBAT_FULL_MV)  return 100;
    return (vbat_mv - VBAT_EMPTY_MV) * 100 / (VBAT_FULL_MV - VBAT_EMPTY_MV);
}

// Periodic battery poll (called from the ~60 s heartbeat). Logs VBAT and drives
// the low-battery latch with hysteresis: warn once on the way down, clear only
// after a solid recovery.
void app_sm_batt_check(void)
{
    int mv = app_sm_read_vbat_mv();
    if (mv < 0) return;
    int pct = app_sm_batt_pct(mv);
    if (!s_batt_low && mv < VBAT_LOW_MV) {
        s_batt_low = true;
        ESP_LOGW(TAG, "battery LOW: VBAT=%d mV (~%d%%)", mv, pct);
    } else if (s_batt_low && mv > VBAT_LOW_MV + VBAT_LOW_HYST_MV) {
        s_batt_low = false;
        ESP_LOGI(TAG, "battery recovered: VBAT=%d mV (~%d%%)", mv, pct);
    } else {
        ESP_LOGI(TAG, "battery: VBAT=%d mV (~%d%%)%s",
                 mv, pct, s_batt_low ? " [LOW]" : "");
    }
}

// Apply the wheel reading to the active path's volume, with hysteresis. No-op
// when the wheel is disabled (default) or the reading hasn't moved enough.
static void vol_apply_from_wheel(void)
{
    if (!s_wheel_enabled) return;
    int pct = vol_read_pct_smoothed();
    if (pct < 0) return;
    if (s_vol_last_pct >= 0 &&
        (pct - s_vol_last_pct < VOL_HYSTERESIS_PCT) &&
        (s_vol_last_pct - pct < VOL_HYSTERESIS_PCT)) {
        return;   // within hysteresis band, ignore
    }
    s_vol_last_pct = pct;
    if (s_active_mode == ES8388_MODE_BYPASS) {
        es8388_set_output_volume(pct);        // Mode A: codec output drivers (I2C)
    } else if (s_state == ST_STREAMING) {
        settings_set_bt_volume((uint8_t)pct); // BT sink carries the audio (local amp
                                              // muted in STREAMING): drive BT volume
    } else {
        settings_set_volume((uint8_t)pct);    // Mode B: local-speaker DSP volume
    }
}

void app_sm_set_wheel_enabled(bool enabled)
{
    s_wheel_enabled = enabled;
    s_vol_last_pct  = -1;     // force the next poll to apply
    s_wheel_ema     = -1.0f;  // re-seed the smoother on re-enable
    ESP_LOGI(TAG, "VOL wheel %s", enabled ? "enabled" : "disabled");
}

// Dedicated Mode B VOL-wheel poll. The wheel used to be sampled from the 200 ms
// software-timer callback, which runs on the priority-1 FreeRTOS timer daemon.
// During A2DP streaming both cores are saturated by higher-priority work (BT
// controller + Bluedroid on core 0; audio_pipe prio 10 and sfx_feed prio 8 on
// core 1), so that daemon is starved and the poll stretches to hundreds of ms --
// the wheel lag the user feels. This task is pinned to core 1 at priority 5:
// below the audio producer and sfx (which spend most of each block blocked on the
// I2S DMA, leaving ample slack), but far above the timer daemon, so the cadence
// stays regular regardless of BT load. In Mode B, vol_apply_from_wheel only takes
// the thread-safe settings lock -- it never touches the codec I2C (that branch is
// Mode-A-only, and s_active_mode never leaves DSP in a Mode B session), so
// applying directly from here introduces no new cross-task race. Only created in
// Mode B; Mode A owns the CPU in mode_a_run() with its own light-sleep wheel poll,
// and a competing core-1 task would defeat the sleep.
static void vol_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(VOL_POLL_MS));
        if (s_state == ST_OTA) continue;   // don't churn volume mid-flash
        vol_apply_from_wheel();
    }
}

void app_sm_prime_volume(void)
{
    // Bring up the ADC and seed the speaker volume from the wheel BEFORE the first
    // audio block, so boot audio starts at the wheel setting instead of the stored
    // default. Without this the wheel isn't read until app_sm_start()'s poll (now
    // behind the BT hold-off, seconds out), and a wheel-down user hears the default
    // volume blare on every power-cycle. The codec boots in Mode B (DSP), so the
    // seed goes to the DSP volume; app_sm_start()'s poll takes over from here.
    vol_adc_init();
    if (!s_wheel_enabled) return;
    int pct = vol_read_pct();          // one unsmoothed oversampled read (~1-2 ms)
    if (pct < 0) return;               // ADC unavailable; keep the stored default
    s_vol_last_pct = pct;              // seed the poll's hysteresis reference
    settings_set_volume((uint8_t)pct);
    ESP_LOGI(TAG, "boot volume primed from wheel: %d%%", pct);
}

// ---- event source callbacks ----------------------------------------------

static void on_button(btn_event_t ev)
{
    switch (ev) {
    case BTN_EV_CP_SHORT:        post(EV_BTN_CP_SHORT);        break;
    case BTN_EV_CP_CONNECT:      post(EV_BTN_CP_CONNECT);      break;
    case BTN_EV_CP_PAIR:         post(EV_BTN_CP_PAIR);         break;
    case BTN_EV_CP_MODE:         post(EV_BTN_CP_MODE);         break;
    case BTN_EV_CP_ZONE_CONNECT: post(EV_BTN_CP_ZONE_CONNECT); break;
    case BTN_EV_CP_ZONE_PAIR:    post(EV_BTN_CP_ZONE_PAIR);    break;
    case BTN_EV_CP_ZONE_MODE:    post(EV_BTN_CP_ZONE_MODE);    break;
    // Drop real jack edges while the HP state is forced (debug lock).
    case BTN_EV_HP_PLUG:    if (s_hp_force < 0) post(EV_BTN_HP_PLUG);   break;
    case BTN_EV_HP_UNPLUG:  if (s_hp_force < 0) post(EV_BTN_HP_UNPLUG); break;
    }
}

// Debug/bench: override HP-detect (the `hp` console command). mode 1 = force
// plugged, 0 = force unplugged, -1 = follow the real GPIO again. Reuses the normal
// HP-event path (so amp mute + codec routing behave exactly as a real plug), then
// on release re-syncs to the live pin level.
void app_sm_force_hp(int mode)
{
    s_hp_force = mode;
    bool plugged = (mode >= 0) ? (mode == 1)
                               : (gpio_get_level(PIN_HP_DETECT) == 1);
    ESP_LOGI(TAG, "HP override: %s",
             mode < 0 ? "follow GPIO" : (mode ? "forced PLUGGED" : "forced UNPLUGGED"));
    post(plugged ? EV_BTN_HP_PLUG : EV_BTN_HP_UNPLUG);
}

static void on_bt(const bt_event_msg_t *m)
{
    switch (m->type) {
    case BT_EV_CONNECTED:        post(EV_BT_CONNECTED);        break;
    case BT_EV_DISCONNECTED:     post(EV_BT_DISCONNECTED);     break;
    case BT_EV_AUDIO_STARTED:    post(EV_BT_AUDIO_STARTED);    break;
    case BT_EV_AUDIO_SUSPENDED:  post(EV_BT_AUDIO_SUSPENDED);  break;
    case BT_EV_PAIRING_TIMEOUT:  post(EV_BT_PAIRING_TIMEOUT);  break;
    }
}

static void on_silence(bool silent)
{
    post(silent ? EV_AUDIO_SILENT : EV_AUDIO_ACTIVE);
}

// Noise-gate deep-silence transition (called from the audio task). Record it and
// poke the SM to re-evaluate the speaker-amp mute.
static void on_gate_mute(bool muting)
{
    s_gate_muting = muting;
    post(EV_GATE_CHANGED);
}

// BLE OTA lifecycle (ble_config). Fires on the Bluedroid task; post into the SM
// queue so the quiesce/resume runs in the SM's single context with no race on
// s_state. BEGIN_REQUEST means "client wants to flash; stop audio then call
// ble_config_ota_proceed()"; FINISHED means a non-reboot end (abort / error /
// disconnect / stall) so we resume normal operation.
static void on_ota(ble_ota_event_t ev)
{
    post(ev == BLE_OTA_EV_BEGIN_REQUEST ? EV_OTA_BEGIN : EV_OTA_END);
}

static void standby_to_cb   (TimerHandle_t t) { post(EV_STANDBY_TIMEOUT);    }
static void reconnect_cb    (TimerHandle_t t) { post(EV_RECONNECT_TICK);     }
static void pairing_to_cb   (TimerHandle_t t) { post(EV_PAIRING_SESSION_TIMEOUT); }

// ---- transitions ----------------------------------------------------------

static void enter(app_state_t next);

static void try_connect_or_pair(void)
{
    esp_err_t err = bt_a2d_connect_bonded();
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "no bonded peer; entering pairing");
        enter(ST_PAIRING);
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "connect_bonded err=%d; will retry", err);
    }
}

static void enter(app_state_t next)
{
    if (next == s_state) return;
    app_state_t prev = s_state;
    ESP_LOGI(TAG, "*** %s -> %s ***", state_name(prev), state_name(next));

    // Exit-side cleanup of the OLD state.
    switch (prev) {
    case ST_STANDBY:
        xTimerStop(s_standby_to_t, 0);
        xTimerStop(s_reconnect_t, 0);
        break;
    case ST_PAIRING:
        xTimerStop(s_pairing_to_t, 0);
        break;
    case ST_STREAMING:
        pam_set_unmuted(true);
        break;
    case ST_MODE_A:
        // Leaving Mode A: restore the digital path. Restart I2S (MCLK live) and
        // resume the pipeline first so the ADC DMA is serviced again, then flip
        // the codec back to DSP. Live MCLK with unserviced DMA storms the
        // watchdog. Write-only codec switch, no verify on this hot path. The codec
        // kept its registers across the MCLK stop, so set_mode(DSP) suffices (no
        // full es8388_init).
        audio_pipeline_resume();
        es8388_set_mode(ES8388_MODE_DSP);
        es8388_route_output(ES8388_OUT_BOTH);
        s_active_mode = ES8388_MODE_DSP;
        break;
    case ST_OTA:
        // Leaving OTA without a reboot (abort/error/disconnect/stall): restart the
        // digital path. The codec was never moved out of DSP mode during OTA, so
        // resuming the pipeline (which restarts I2S) is all that's needed, no
        // es8388 calls. pam_apply() below re-unmutes as appropriate.
        audio_pipeline_resume();
        break;
    default:
        break;
    }

    s_state = next;
    pam_apply();

    // Entry-side actions of the NEW state.
    switch (next) {
    case ST_STANDBY:
        // Disconnect cue if we just lost a live link (so the user knows the
        // sink dropped). Plays on the local speaker; BT is gone.
        if (prev == ST_CONNECTED_IDLE || prev == ST_STREAMING) {
            CUE(SFX_SYNTH_DISCONNECT);
        }
        // Auto-connect gate. Fresh paging of bonded sinks is an automatic BT
        // action, so with auto_connect off we don't initiate it from an idle
        // start. Two entries are exempt and always (re)connect regardless of the
        // setting:
        //   - prev == ST_LOCAL_ONLY: the user explicitly asked, via a Connect hold
        //     (handle_local_only EV_BTN_CP_CONNECT).
        //   - prev == ST_CONNECTED_IDLE / ST_STREAMING: a link the user already
        //     had dropped (e.g. mid-stream), so auto-reconnect it. The reconnect
        //     runs via the deferred timer below, and STANDBY_TIMEOUT falls to
        //     LOCAL_ONLY if it can't get the sink back.
        // Every other automatic entry (Mode A exit, a pairing-session timeout)
        // waits in LOCAL_ONLY for a Connect/Pair hold instead of paging. Boot is
        // gated separately in sm_task (it doesn't run this entry code).
        {
            gbhifi_settings_t gate_s;
            settings_get(&gate_s);
            if (!gate_s.auto_connect && prev != ST_LOCAL_ONLY &&
                prev != ST_CONNECTED_IDLE && prev != ST_STREAMING) {
                ESP_LOGI(TAG, "STANDBY: auto-connect off; to LOCAL_ONLY (await Connect/Pair hold)");
                enter(ST_LOCAL_ONLY);
                break;
            }
        }
        // When we land in STANDBY straight off a live link (the sink dropped, or
        // A2DP closed), Bluedroid is still tearing down that ACL link in the BTU
        // task. Issuing a fresh direct-page now races l2cu_release_lcb freeing the
        // old link's timer node and corrupts the BT heap (StoreProhibited in
        // tlsf_free). Defer the first page to the periodic reconnect timer, which
        // fires well after the link has released. Boot and other entries have no
        // teardown in flight, so page immediately for a fast reconnect.
        if (prev != ST_CONNECTED_IDLE && prev != ST_STREAMING) {
            try_connect_or_pair();
        }
        if (s_state != ST_PAIRING) {  // didn't transfer to PAIRING above
            xTimerStart(s_standby_to_t, 0);
            xTimerStart(s_reconnect_t,  0);
        }
        break;
    case ST_PAIRING:
        // Bound the high-power inquiry/paging window: start the one-shot
        // session timer on entry (stopped on exit above). Retries while
        // pairing stay inside this state (see handle_pairing) so the timer
        // measures the whole session rather than being reset each retry.
        xTimerStart(s_pairing_to_t, 0);
        // Fresh session: forget any sinks blacklisted for failing to open A2DP
        // in a previous session, so they get a clean chance again. Failures
        // within this session are still remembered and skipped across re-scans.
        bt_a2d_reset_fail_blacklist();
        bt_a2d_start_pairing();
        // amp is live in PAIRING, so the user hears it — but skip it on the
        // power-on arrival so we don't stomp the Game Boy boot chime.
        if (s_boot_arrival) s_boot_arrival = false;
        else                CUE(SFX_SYNTH_PAIRING);
        break;
    case ST_CONNECTED_IDLE:
        // Connect cue on a fresh connection (from STANDBY reconnect or a
        // just-completed pairing), not on the STREAMING to CI silence drop.
        if (prev == ST_STANDBY || prev == ST_PAIRING) {
            // Skip the cue on the power-on reconnect so it doesn't stomp the
            // Game Boy boot chime; still cue on later reconnects.
            if (s_boot_arrival) s_boot_arrival = false;
            else                CUE(SFX_SYNTH_CONNECT);
        }
        // Silence callback only fires on edges. Coming from a fresh connection
        // (STANDBY boot/reconnect or PAIRING just-paired), audio may already be
        // flowing with no observable edge, so re-prime to promote to STREAMING.
        // Coming from STREAMING (silence or sink-suspended), we deliberately stay
        // here; re-priming would cause a STREAMING to CI to STREAMING flicker.
        if ((prev == ST_STANDBY || prev == ST_PAIRING) &&
            !s_hp_plugged && !audio_pipeline_is_silent()) {
            post(EV_AUDIO_ACTIVE);
        }
        break;
    case ST_STREAMING:
        bt_a2d_media_start();
        break;
    case ST_DEEP_IDLE:
        // True ESP32 deep sleep (chip resets on wake). Reached only via the console
        // `sleep` command (EV_FORCE_SLEEP), a wake-reliability bench test. The
        // ES8388 is the HP amp on the digital path, so deep-sleeping silences wired
        // headphones; idle-with-HP normally stays in LOCAL_ONLY or Mode A. Both
        // wake sources are configured here; the next boot logs the cause.
        //
        // ESP32 ext1 only supports ALL_LOW or ANY_HIGH (no mixed polarity). R wakes
        // on LOW, HP wakes on HIGH, so split across ext0 (HP/HIGH) and ext1
        // (R/single-pin ALL_LOW = that pin LOW). Both can coexist.
        //
        // The pam_apply() call above already set PIN_PAM_SD to the right value for
        // this state + HP combo. The subsequent gpio_hold_en freezes that exact
        // level across sleep, so the speaker keeps playing GBA audio (HP unplugged).
        // With HP plugged this test path does silence the headphones (the ES8388 HP
        // amp stops in deep sleep); acceptable only because this state is
        // bench-test-only.
        bt_a2d_disconnect();
        // Brief settle so any in-flight BT shutdown packets can run before the chip
        // slams into deep sleep.
        vTaskDelay(pdMS_TO_TICKS(100));

        // Freeze PIN_PAM_SD HIGH across deep sleep. Without this the pad goes
        // high-Z when the chip sleeps; the amp's SD pin has a weak internal
        // pull-down that then wins, muting the amp. DEEP_IDLE means BT off but
        // speaker still carrying the GBA's analog audio, so the amp must stay on.
        ESP_ERROR_CHECK(gpio_hold_en(PIN_PAM_SD));
        gpio_deep_sleep_hold_en();

        // ext0 is level-triggered, not edge-triggered: it fires whenever the pin is
        // at the configured level. If HP is currently HIGH and we configured
        // wake-on-HIGH, the chip would wake instantly and loop forever. So wake on
        // the opposite of the current level: whichever way HP next transitions
        // wakes us.
        int hp_wake_level = s_hp_plugged ? 0 : 1;
        ESP_LOGI(TAG, "configuring wake sources (HP wake on %s) and entering DEEP SLEEP",
                 hp_wake_level ? "plug HIGH" : "unplug LOW");
        ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(PIN_HP_DETECT, hp_wake_level));
        ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup(
            1ULL << PIN_CP_BUTTON, ESP_EXT1_WAKEUP_ALL_LOW));
        esp_deep_sleep_start();
        // Unreachable: the call above resets the chip.
        break;
    case ST_LOCAL_ONLY:
        // Local speaker only: the BT reconnect window expired (or pairing gave up
        // with no bonds) while the speaker is the active output. Deep-sleeping
        // would silence the digital path and there's no audio wake source, so
        // instead stop reconnect paging and stay awake carrying GBA audio. The
        // reconnect/standby timers were already stopped on the STANDBY exit side,
        // so the radio sits idle (modem-sleep); we deliberately keep it
        // initialized so a Connect/Pair hold can act live (handle_local_only).
        // pam_apply() above left the amp unmuted (not STREAMING, HP unplugged). No
        // timers: terminal until a Connect/Pair hold or an HP plug. The mod is on
        // the GBA's switched rail, so there is no battery-drain reason to
        // auto-sleep (GBA off cuts our power entirely).
        ESP_LOGI(TAG, "local-speaker-only; BT idle, reconnect paused; amp stays on");
        break;
    case ST_MODE_A:
        // Mode A (analog bypass / battery saver). This is just the setup; the
        // duty-cycled light-sleep loop runs in mode_a_run() (invoked by the SM task
        // right after this enter() returns). Order matters: do the codec writes
        // while MCLK is still live and the pipeline still services the DMA, then
        // park the pipeline + stop I2S.
        //  1. Disconnect any A2DP link (Mode A has no BT audio). The controller is
        //     left up; the manual light sleep works with it enabled.
        //  2. Mute the speaker amp across the routing flip (no pop).
        //  3. Set codec output volume from the wheel (if enabled).
        //  4. Switch the codec to analog bypass (write-only).
        //  5. Route to HP or speaker per the current jack state.
        //  6. Unmute the amp iff HP is unplugged.
        //  7. Park the pipeline, i2s_codec_stop() (MCLK/BCLK now stop; the bypass
        //     keeps playing in hardware with no clock).
        // Suppress buttons.c event emission for the whole Mode A session: the
        // light-sleep loop reads the R button + HP directly, and a stray debounced
        // release event racing past the exit would re-toggle the mode.
        buttons_set_emit_enabled(false);
        bt_a2d_disconnect();
        pam_set_unmuted(false);
        if (s_wheel_enabled) {
            int pct = vol_read_pct();
            if (pct >= 0) es8388_set_output_volume(pct);
        }
        es8388_set_mode(ES8388_MODE_BYPASS);
        es8388_route_output(s_hp_plugged ? ES8388_OUT_HEADPHONE
                                         : ES8388_OUT_SPEAKER);
        s_active_mode = ES8388_MODE_BYPASS;
        pam_set_unmuted(!s_hp_plugged);
        audio_pipeline_stop();
        ESP_LOGI(TAG, "Mode A set up; analog bypass, digital path stopped; entering light sleep");
        break;
    case ST_OTA:
        // Quiesce the audio path before any flash work. The per-sector erases and
        // chunk writes (flash cache disabled) would otherwise starve the SBC
        // encoder and garble the local DAC. Order: drop A2DP, stop the pipeline
        // (parks the task + stops I2S), mute the speaker amp (pam_apply above
        // would have unmuted it; re-mute for the flash). The BLE controller runs
        // from IRAM, so the OTA link itself survives the cache-disable. Then
        // ble_config_ota_proceed() does esp_ota_begin() + READY and the transfer
        // runs on the Bluedroid task. The codec stays in DSP mode untouched, so
        // an abort just needs the pipeline back.
        bt_a2d_disconnect();
        audio_pipeline_stop();
        pam_set_unmuted(false);
        ble_config_ota_proceed();
        break;
    }
}

// ---- state-specific handlers ---------------------------------------------

static void handle_standby(sm_event_t ev)
{
    switch (ev) {
    case EV_BT_CONNECTED:
        enter(ST_CONNECTED_IDLE);
        break;
    case EV_BTN_CP_CONNECT:
        // Released in the connect zone: re-page the bonded sinks now (we're already
        // in the reconnect campaign; this just kicks it immediately). The
        // connect-rung chime already fired as the hold crossed the zone.
        try_connect_or_pair();
        break;
    case EV_BTN_CP_PAIR:
        enter(ST_PAIRING);
        break;
    case EV_STANDBY_TIMEOUT:
        // Reconnect window spent. The digital speaker runs through the ESP32, so
        // deep-sleeping would silence it (and there's no audio wake source). Drop
        // the radio to idle and stay awake carrying audio; see the ST_LOCAL_ONLY
        // entry.
        ESP_LOGI(TAG, "standby reconnect timeout (%d s); to LOCAL_ONLY",
                 CONFIG_GBHIFI_STANDBY_TIMEOUT_S);
        enter(ST_LOCAL_ONLY);
        break;
    case EV_RECONNECT_TICK:
        try_connect_or_pair();
        break;
    default:
        break;
    }
}

static void handle_pairing(sm_event_t ev)
{
    switch (ev) {
    case EV_BT_CONNECTED:
        enter(ST_CONNECTED_IDLE);
        break;
    case EV_BT_PAIRING_TIMEOUT:
        // Inquiry finished with no matching sink. Re-inquire and stay in
        // PAIRING; the session timer (not this per-inquiry event) bounds
        // how long we keep trying.
        ESP_LOGI(TAG, "no sink found; re-scanning");
        bt_a2d_start_pairing();
        break;
    case EV_BT_DISCONNECTED:
        // A matched sink was paged but refused/failed the A2DP open. Re-scan and
        // stay in PAIRING so the session timer keeps bounding the high-power window.
        ESP_LOGI(TAG, "pairing page failed; re-scanning");
        bt_a2d_start_pairing();
        break;
    case EV_PAIRING_SESSION_TIMEOUT:
        // Session budget spent. Stop burning power on inquiry: sleep if we
        // have nothing bonded to fall back to, otherwise resume bonded
        // reconnect. A held-R (or HP) wake starts a fresh pairing session.
        if (bt_a2d_has_bond()) {
            ESP_LOGI(TAG, "pairing timed out; bonds exist, back to STANDBY");
            enter(ST_STANDBY);
        } else {
            // No bonds: fall to LOCAL_ONLY and keep carrying GBA audio out the
            // digital path (ES8388 to HP amp + speaker). We do not deep-sleep even
            // with HP plugged: the ES8388 is the HP amp, so deep sleep would
            // silence the wired headphones too. LOCAL_ONLY keeps the ESP32 driving
            // both outputs; pam_apply mutes the external speaker when HP is plugged.
            ESP_LOGI(TAG, "pairing timed out; no bonds; to LOCAL_ONLY (HP %s)",
                     s_hp_plugged ? "plugged" : "unplugged");
            enter(ST_LOCAL_ONLY);
        }
        break;
    default:
        break;
    }
}

static void handle_connected_idle(sm_event_t ev)
{
    switch (ev) {
    case EV_AUDIO_ACTIVE:
    case EV_BT_AUDIO_STARTED:
        // Wired headphones are plugged into the GBA: keep BT suspended so the user
        // gets zero-latency wired audio. We'll re-promote on unplug if audio is
        // still active.
        if (!s_hp_plugged) {
            enter(ST_STREAMING);
        }
        break;
    case EV_BTN_HP_UNPLUG:
        // Came back from wired listening. If audio is currently flowing, resume BT
        // streaming; otherwise wait for the silence-to-active edge.
        if (!audio_pipeline_is_silent()) {
            post(EV_AUDIO_ACTIVE);
        }
        break;
    case EV_BT_DISCONNECTED:
        enter(ST_STANDBY);
        break;
    default:
        break;
    }
}

static void handle_streaming(sm_event_t ev)
{
    switch (ev) {
    case EV_AUDIO_SILENT:
    case EV_BT_AUDIO_SUSPENDED:
        bt_a2d_media_suspend();
        enter(ST_CONNECTED_IDLE);
        break;
    case EV_BTN_HP_PLUG:
        // Hot-swap: user plugged wired HP into the GBA jack. Hand the audio off to
        // the wired path by suspending BT and dropping to CONNECTED_IDLE. The
        // s_hp_plugged guard in CI promotion keeps us there until they unplug.
        ESP_LOGI(TAG, "wired HP plugged during STREAMING; suspending A2DP");
        bt_a2d_media_suspend();
        enter(ST_CONNECTED_IDLE);
        break;
    case EV_BT_DISCONNECTED:
        enter(ST_STANDBY);
        break;
    default:
        break;
    }
}

static void handle_deep_idle(sm_event_t ev)
{
    switch (ev) {
    case EV_BTN_CP_SHORT:
    case EV_BTN_HP_PLUG:
        ESP_LOGI(TAG, "wake from DEEP_IDLE (event %d)", ev);
        enter(ST_STANDBY);
        break;
    default:
        break;
    }
}

static void handle_local_only(sm_event_t ev)
{
    switch (ev) {
    case EV_BTN_CP_CONNECT:
        // Released in the connect zone: the radio is still up (we only stopped
        // paging), so resume the reconnect campaign live, no reboot. Back to
        // STANDBY which re-pages the bonds + restarts the timers. The connect-rung
        // chime already fired as the hold crossed the zone.
        enter(ST_STANDBY);
        break;
    case EV_BTN_CP_PAIR:
        // Pairing hold: go discover a new sink (PAIRING entry plays its cue).
        enter(ST_PAIRING);
        break;
    default:
        // HP plug/unplug is tracked centrally in sm_task (s_hp_plugged + PAM).
        // A mode-toggle hold is handled centrally too. Everything else is
        // ignored: there's no live link in this state, so no BT events arrive.
        break;
    }
}

// ---- Mode A duty-cycled light-sleep loop -----------------------------------
// The whole Mode A lifecycle lives here, not in the event loop. enter(ST_MODE_A)
// has already parked the pipeline + put the codec in analog bypass; this loop then
// light-sleeps the CPU, waking ~every MODE_A_POLL_MS to track the volume wheel, on
// an HP edge to reroute, or on an R-button press to time a hold. It returns only
// when Mode A exits, transitioning back to the digital path itself before
// returning, so the caller resumes the normal event loop in a well-defined state.
// The BT controller stays up throughout (no teardown).

// Time an R-button hold while the CPU is awake (the buttons.c FreeRTOS timers are
// frozen during light sleep, so the menu can't run in Mode A; exit is a plain
// timed hold). Returns true once held continuously for hold_mode_exit_ms; false if
// released sooner (a benign gameplay tap, re-sleep).
static bool mode_a_button_hold(void)
{
    gbhifi_settings_t s;
    settings_get(&s);
    int64_t t0 = esp_timer_get_time();
    while (gpio_get_level(PIN_CP_BUTTON) == 0) {   // active LOW = still pressed
        if ((esp_timer_get_time() - t0) / 1000 >= (int64_t)s.hold_mode_exit_ms) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}

static void mode_a_run(void)
{
    ESP_LOGI(TAG, "Mode A: light-sleep duty loop (poll %d ms; R-hold to exit)",
             MODE_A_POLL_MS);

    // The project runs the PM framework (CONFIG_PM_ENABLE) with automatic light
    // sleep OFF, so Mode A must hand-roll esp_light_sleep_start(). A manual sleep
    // ignores PM locks, so we have to make the peripherals safe ourselves:
    //
    //  1. BT: enter(ST_MODE_A) already called bt_a2d_disconnect(), but that only
    //     *starts* the teardown; the ACL disconnect completes asynchronously on
    //     the BT work-task, which can't run while we're asleep. Sleeping on a live
    //     ACL hangs the sleep transition and the RTC watchdog resets the SoC
    //     (rst:0x10). Stay awake (spin) until the link is fully down.
    const int BT_QUIESCE_TIMEOUT_MS = 1500;
    int64_t bt_t0 = esp_timer_get_time();
    while (bt_a2d_link_active()) {
        if ((esp_timer_get_time() - bt_t0) / 1000 >= BT_QUIESCE_TIMEOUT_MS) {
            ESP_LOGW(TAG, "Mode A: BT link still active after %d ms; sleeping anyway",
                     BT_QUIESCE_TIMEOUT_MS);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGI(TAG, "Mode A: BT quiesced (%lld ms); entering light sleep",
             (esp_timer_get_time() - bt_t0) / 1000);

    int  last_pct = -1;
    bool exit_mode_a = false;

    while (!exit_mode_a) {
        // Arm the wake sources fresh each cycle: ~200 ms timer (wheel poll), ext0
        // on the next HP-detect transition, ext1 on the R button LOW. The ext0
        // level is the opposite of the pin's current level so we wake on the edge,
        // not instantly (same polarity trick as DEEP_IDLE).
        esp_sleep_enable_timer_wakeup((uint64_t)MODE_A_POLL_MS * 1000);
        int hp_wake_level = (gpio_get_level(PIN_HP_DETECT) == 1) ? 0 : 1;
        esp_sleep_enable_ext0_wakeup(PIN_HP_DETECT, hp_wake_level);
        esp_sleep_enable_ext1_wakeup(1ULL << PIN_CP_BUTTON, ESP_EXT1_WAKEUP_ALL_LOW);

        //  2. Console UART: never sleep with a reply still shifting out. Cutting
        //     the UART clock mid-TX corrupts the transfer and wedges the sleep
        //     transition -> RTCWDT. (We don't make UART a wake source: that eats
        //     the first RX edges and races the async console task against the duty
        //     loop; a command still lands when sent during an awake window.)
        esp_rom_output_tx_wait_idle(CONFIG_ESP_CONSOLE_UART_NUM);

        esp_light_sleep_start();
        esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

        if (cause == ESP_SLEEP_WAKEUP_TIMER) {
            // Live volume: poll the (smoothed) wheel, write the codec only on a
            // real move past the hysteresis band.
            if (s_wheel_enabled) {
                int pct = vol_read_pct_smoothed();
                if (pct >= 0 &&
                    (last_pct < 0 ||
                     pct - last_pct >= VOL_HYSTERESIS_PCT ||
                     last_pct - pct >= VOL_HYSTERESIS_PCT)) {
                    es8388_set_output_volume(pct);
                    last_pct = pct;
                }
            }

        } else if (cause == ESP_SLEEP_WAKEUP_EXT0) {
            // HP-detect edge. Debounce, re-read, reroute the codec + amp. We do our
            // own HP bookkeeping here (the central sm_task loop isn't running while
            // we're in this loop).
            vTaskDelay(pdMS_TO_TICKS(CONFIG_GBHIFI_DEBOUNCE_MS));
            bool plugged = (s_hp_force >= 0) ? (s_hp_force == 1)
                                             : (gpio_get_level(PIN_HP_DETECT) == 1);
            if (plugged != s_hp_plugged) {
                s_hp_plugged = plugged;
                ESP_LOGI(TAG, "Mode A: HP %s", plugged ? "plug" : "unplug");
                es8388_route_output(plugged ? ES8388_OUT_HEADPHONE
                                            : ES8388_OUT_SPEAKER);
                pam_set_unmuted(!plugged);
                dsp_set_hp_plugged(plugged); // right EQ profile live on Mode B resume
                last_pct = -1;   // re-apply volume after the route change
                // Mode A stays put on a wired-HP unplug (speaker keeps running in
                // battery mode, symmetric with insertion); leaving Mode A is only
                // an explicit R-hold or a mode change.
            }

        } else if (cause == ESP_SLEEP_WAKEUP_EXT1) {
            // R-button press. Time the hold awake; exit to Mode B the instant it
            // reaches the threshold (fire at deadline, not on release). Mode A has
            // no chime menu, so the user can't hear how long they've held; exiting
            // at the deadline gives a definite cue (DSP audio + the "full mode"
            // chime return). A shorter release is a benign tap: fall through and
            // re-sleep. buttons.c emission is suppressed for the whole Mode A
            // session, so the still-held button can't fire a stray action.
            if (mode_a_button_hold()) {
                ESP_LOGI(TAG, "Mode A: R held to threshold; rebooting into Mode B");
                app_sm_switch_mode(false);   // save B + reboot; does not return
            }
        }
    }

    // Disarm the wake sources so they don't linger into the awake states.
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT0);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT1);

    // Arming ext0/ext1 routed GPIO34/35 through the RTC mux; reclaim them for
    // the digital edge ISRs so buttons.c works again in Mode B.
    buttons_refresh_gpio();

    // Reset any hold buttons.c silently timed during the suppressed window, then
    // flush the SM queue, then re-enable emission. Order matters: emission stays
    // off until buttons.c is back in a released/idle state, so the exit hold can't
    // fire a stray action (e.g. re-toggle the mode).
    buttons_cancel_hold();
    xQueueReset(s_evq);
    buttons_set_emit_enabled(true);

    // Exit: enter(ST_STANDBY) runs the ST_MODE_A exit-side cleanup (resume the
    // pipeline, restore the DSP codec + both routing) and re-arms the reconnect
    // campaign on the still-up radio. Then a "full mode" cue, now that the DSP path
    // (and SFX) is alive again.
    enter(ST_STANDBY);
    CUE(SFX_SYNTH_MODE);
    ESP_LOGI(TAG, "Mode A exited; Mode B / STANDBY");
}

// ---- SM task --------------------------------------------------------------

static void sm_task(void *arg)
{
    // Sample HP-detect once at boot. buttons.c has already configured the
    // pin as an input by this point; without this sample we'd start with
    // s_hp_plugged=false even if the user booted with wired headphones
    // already plugged in.
    s_hp_plugged = (gpio_get_level(PIN_HP_DETECT) == 1);
    ESP_LOGI(TAG, "initial HP state: %s",
             s_hp_plugged ? "plugged" : "unplugged");
    pam_apply();
    dsp_set_hp_plugged(s_hp_plugged);   // seed the local-EQ profile for boot HP state

    // Boot transition: ST_STANDBY entry. s_state is already ST_STANDBY, so call
    // the entry actions explicitly. main.c already made the boot-mode decision
    // (settings + reset reason + CP-hold) and gated BT init on it; s_boot_into_mode_a
    // tells us to match. Mode changes at runtime reboot into the target mode, so the
    // only place Mode A is entered is here at boot.
    ESP_LOGI(TAG, "*** boot -> %s ***", state_name(s_state));
    gbhifi_settings_t boot_s;
    settings_get(&boot_s);
    if (s_boot_into_mode_a) {
        // Mode A boot (main.c skipped BT init): drop straight into the light-sleep
        // loop -- no window, no pause. mode_a_run() owns the CPU and only ever leaves
        // Mode A by rebooting, so it does not return. Holding CP (R) at power-on
        // already diverted this boot to Mode B in main.c; that is the only escape.
        ESP_LOGI(TAG, "boot: entering Mode A");
        s_boot_arrival = false;
        enter(ST_MODE_A);
        mode_a_run();
    } else if (boot_s.auto_connect) {
        try_connect_or_pair();
        if (s_state == ST_STANDBY) {
            xTimerStart(s_standby_to_t, 0);
            xTimerStart(s_reconnect_t,  0);
        }
    } else {
        // Manual BT (default): don't page/inquire on boot. Rest in LOCAL_ONLY
        // with the radio idle-but-initialized, carrying local GBA audio out the
        // digital path; a Connect/Pair hold starts the link live from there (see
        // handle_local_only). Consume the boot-arrival flag so a later
        // user-initiated connect/pair plays its cue (there's no boot chime to
        // stomp by the time the user holds the button).
        ESP_LOGI(TAG, "boot: BT auto-connect off; waiting for Connect/Pair hold");
        s_boot_arrival = false;
        enter(ST_LOCAL_ONLY);
    }

    for (;;) {
        uint8_t e;
        if (xQueueReceive(s_evq, &e, portMAX_DELAY) != pdTRUE) continue;
        sm_event_t ev = (sm_event_t)e;
        // Track HP plug state across all transitions before per-state dispatch:
        // the state handlers consult s_hp_plugged. The amp also re-evaluates
        // immediately so the speaker mutes/unmutes on the plug edge even before any
        // state transition fires. Idle with HP plugged just keeps playing (the
        // ES8388 is the HP amp), in LOCAL_ONLY or Mode A.
        if (ev == EV_BTN_HP_PLUG) {
            s_hp_plugged = true;
            pam_apply();
            dsp_set_hp_plugged(true);   // swap the local EQ to the Headphone profile
        }
        if (ev == EV_BTN_HP_UNPLUG) {
            s_hp_plugged = false;
            pam_apply();
            dsp_set_hp_plugged(false);  // swap the local EQ to the Speaker profile
        }
        // Hold-menu zone-feedback chimes fire as the hold crosses each rung (the
        // menu is self-documenting: "hold till the beep you want"). The action
        // fires separately on release. Speaker-path cues, so inaudible while
        // STREAMING (amp muted) or in Mode A (DSP/SFX stopped); fine.
        if (ev == EV_BTN_CP_ZONE_CONNECT) { CUE(SFX_SYNTH_REBOND);  continue; }
        if (ev == EV_BTN_CP_ZONE_PAIR)    { CUE(SFX_SYNTH_PAIRING); continue; }
        if (ev == EV_BTN_CP_ZONE_MODE)    { CUE(SFX_SYNTH_MODE);    continue; }
        // Mode-toggle (released in the mode rung): switch by rebooting into the
        // other mode. A Mode A boot comes up BT-less (clean, low-power light sleep);
        // Mode B boots with the radio.
        if (ev == EV_BTN_CP_MODE) {
            gbhifi_settings_t s;
            settings_get(&s);
            ESP_LOGI(TAG, "mode-toggle hold; switching to Mode %c (reboot)",
                     s.mode_a ? 'B' : 'A');
            app_sm_switch_mode(!s.mode_a);   // save + reboot; does not return
            continue;
        }
        // Force-sleep (console `sleep` wake-reliability test only). Enter DEEP_IDLE
        // from any awake state except PAIRING (a user flow) and MODE_A (its own
        // loop owns the CPU here).
        if (ev == EV_FORCE_SLEEP &&
            s_state != ST_DEEP_IDLE && s_state != ST_PAIRING &&
            s_state != ST_MODE_A && s_state != ST_OTA) {
            ESP_LOGI(TAG, "force-sleep (bench test); entering DEEP_IDLE");
            enter(ST_DEEP_IDLE);
            continue;
        }
        // Noise-gate deep-silence transition: re-apply the amp mute (drop the
        // speaker amp in silence, restore it the instant audio returns).
        if (ev == EV_GATE_CHANGED) { pam_apply(); continue; }
        // OTA (BLE firmware update). BEGIN: quiesce + enter ST_OTA, but only from
        // an awake, non-special state: Mode A blocks this task entirely, and
        // PAIRING is a bounded user flow. If refused (or never proceeded),
        // ble_config's stall watchdog aborts the request and fires FINISHED.
        // END: a non-reboot OTA end (abort/error/disconnect/stall) resumes.
        if (ev == EV_OTA_BEGIN) {
            if (s_state != ST_OTA && s_state != ST_MODE_A &&
                s_state != ST_PAIRING && s_state != ST_DEEP_IDLE) {
                enter(ST_OTA);
            } else {
                ESP_LOGW(TAG, "OTA begin ignored in state %s", state_name(s_state));
            }
            continue;
        }
        if (ev == EV_OTA_END) {
            if (s_state == ST_OTA) {
                ESP_LOGI(TAG, "OTA ended without reboot; resuming");
                enter(ST_STANDBY);
            }
            continue;
        }
        switch (s_state) {
        case ST_STANDBY:        handle_standby(ev);        break;
        case ST_PAIRING:        handle_pairing(ev);        break;
        case ST_CONNECTED_IDLE: handle_connected_idle(ev); break;
        case ST_STREAMING:      handle_streaming(ev);      break;
        case ST_DEEP_IDLE:      handle_deep_idle(ev);      break;
        case ST_LOCAL_ONLY:     handle_local_only(ev);     break;
        case ST_MODE_A:
            // Unreachable in practice: mode_a_run() owns the CPU while in Mode A
            // and transitions out before returning to this loop. Defensive only.
            break;
        case ST_OTA:
            // OTA lifecycle (begin/end) is handled centrally above; the transfer
            // itself runs on the Bluedroid task. Ignore everything else here,
            // notably the EV_BT_DISCONNECTED from our own bt_a2d_disconnect().
            break;
        }
    }
}

// ---- init -----------------------------------------------------------------

void app_sm_request_sleep(void)
{
    // Force DEEP_IDLE for the deep-sleep/wake-reliability bench test (console
    // `sleep`). The central EV_FORCE_SLEEP handler in sm_task enters DEEP_IDLE from
    // any awake state except PAIRING/MODE_A, configuring wake exactly as a normal
    // sleep. Posted from the console task; the SM acts on it in its own context, so
    // no cross-task race on s_state.
    post(EV_FORCE_SLEEP);
}

void app_sm_switch_mode(bool to_mode_a)
{
    // Mode changes are done by REBOOT, not in place: a Mode A boot skips BT init
    // entirely for a clean, low-power, reliable light-sleep loop, and Mode B comes
    // up with the radio. Persist the target mode, then restart into it. The restart
    // is a software reset (not a power-on), so main.c stays silent (no chime).
    gbhifi_settings_t s;
    settings_get(&s);
    if (s.mode_a == to_mode_a) return;   // already the active mode
    settings_set_mode_a(to_mode_a);
    settings_commit();                   // land the boot in the target mode
    ESP_LOGI(TAG, "mode -> %c; rebooting into it", to_mode_a ? 'A' : 'B');
    esp_rom_output_tx_wait_idle(CONFIG_ESP_CONSOLE_UART_NUM);  // flush the log line
    esp_restart();
}

// main.c computes the boot-mode decision once (settings + reset reason + CP-hold),
// uses it to gate BT init, and hands it here (into s_boot_into_mode_a, declared up
// top) so the state machine drops into the same mode. Set before app_sm_start().
void app_sm_set_boot_into_mode_a(bool v) { s_boot_into_mode_a = v; }

void app_sm_speaker_early_on(void)
{
    // Route the boot chime + early passthrough correctly. The ES8388 DAC feeds BOTH
    // the HP amp and the line-out -> speaker amp, so the speaker amp must come on
    // only when NO headphones are plugged; with headphones in it stays muted and the
    // HP amp alone drives the phones (what pam_apply() enforces steady-state).
    // buttons_init() hasn't configured HP-detect yet, so sample it here; sm_task
    // re-samples it later. Then pam_init() to own PIN_PAM_SD, and pam_apply() to set
    // the HP-aware mute (s_state is still the default ST_STANDBY, gate idle).
    gpio_config_t hp = {
        .pin_bit_mask = 1ULL << PIN_HP_DETECT,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,   // external 10k pull-up on the detect line
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&hp));
    s_hp_plugged = (gpio_get_level(PIN_HP_DETECT) == 1);

    pam_init();
    pam_apply();   // speaker amp on only if HP unplugged (full SM bring-up re-applies later)
}

bool app_sm_hp_plugged(void)
{
    return s_hp_plugged;
}

void app_sm_request_bt_connect(void)
{
    if (!s_evq) { ESP_LOGW(TAG, "bt connect request before app_sm_start"); return; }
    post(EV_BTN_CP_CONNECT);
}

void app_sm_request_bt_pair(void)
{
    if (!s_evq) { ESP_LOGW(TAG, "bt pair request before app_sm_start"); return; }
    post(EV_BTN_CP_PAIR);
}

void app_sm_amp_radio_quiet(bool quiet)
{
    if (quiet) {
        pam_set_unmuted(false);
    } else {
        pam_apply();   // back to the HP/state rule (pre-start: STANDBY + sampled HP)
    }
}

esp_err_t app_sm_start(void)
{
    pam_init();
    vol_adc_init();

    s_evq = xQueueCreate(EVQ_DEPTH, sizeof(uint8_t));
    s_standby_to_t = xTimerCreate("sb_to",
                                  pdMS_TO_TICKS(CONFIG_GBHIFI_STANDBY_TIMEOUT_S * 1000),
                                  pdFALSE, NULL, standby_to_cb);
    s_reconnect_t  = xTimerCreate("rcn",
                                  pdMS_TO_TICKS(CONFIG_GBHIFI_RECONNECT_TICK_S * 1000),
                                  pdTRUE,  NULL, reconnect_cb);
    s_pairing_to_t = xTimerCreate("pair_to",
                                  pdMS_TO_TICKS(CONFIG_GBHIFI_PAIRING_TIMEOUT_S * 1000),
                                  pdFALSE, NULL, pairing_to_cb);

    buttons_set_event_cb(on_button);
    bt_a2d_set_event_cb(on_bt);
    audio_pipeline_set_silence_cb(on_silence);
    dsp_set_gate_mute_cb(on_gate_mute);
    ble_config_set_ota_cb(on_ota);

    xTaskCreate(sm_task, "app_sm", 4096, NULL, 6, NULL);
    // Mode B wheel poll: dedicated task pinned to core 1 (away from the core-0 BT
    // stack) so streaming load can't starve it the way it starved the old prio-1
    // timer-daemon poll. Not created for a Mode A boot -- mode_a_run() owns the CPU
    // and does its own light-sleep wheel poll, and a competing core-1 task would
    // defeat the sleep. (Mode is fixed per boot; a mode toggle reboots.)
    if (!s_boot_into_mode_a) {
        xTaskCreatePinnedToCore(vol_task, "vol_wheel", 3072, NULL, 5, NULL, 1);
    }
    return ESP_OK;
}
