#include "dsp.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "dsps_biquad.h"
#include "dsps_biquad_gen.h"

#include "settings.h"
#include "sfx.h"

static const char *TAG = "dsp";

#define PIPE_RATE   44100.0f
#define MAX_BLOCK   256             // == audio_pipeline READ_FRAMES

// Speaker EQ band centre frequencies, voiced for the small GBA driver: a low
// shelf to tame bass the driver can't reproduce, a presence peak for clarity,
// and a high shelf for air. Band gains come from settings (dB).
#define F_BASS      (150.0f  / PIPE_RATE)
#define F_MID       (2500.0f / PIPE_RATE)
#define F_TREBLE    (5000.0f / PIPE_RATE)
#define Q_SHELF     0.707f
#define Q_PEAK      1.0f

// Per-sample one-pole smoothing for the volume gain gives click-free steps
// (~5 ms time constant at 44.1 kHz).
#define VOL_SMOOTH  0.005f

// Speaker EQ (stereo): coefficients {b0,b1,b2,a1,a2} plus a separate delay line
// {w0,w1} per channel ([0]=L, [1]=R). The local DAC path is stereo (HP), so each
// channel's biquads keep their own state.
static float s_coef_bass[5], s_coef_mid[5], s_coef_treble[5];
static float s_w_bass[2][2], s_w_mid[2][2], s_w_treble[2][2];

// Bluetooth EQ (stereo): same band coefficients, independent gains, and a
// separate delay line per channel ([0]=L, [1]=R). Each biquad instance keeps its
// own state.
static float s_bt_coef_bass[5], s_bt_coef_mid[5], s_bt_coef_treble[5];
static float s_btw_bass[2][2], s_btw_mid[2][2], s_btw_treble[2][2];

static uint32_t s_last_gen = 0xffffffffu;
// Which local-path EQ profile is live: false = Speaker EQ (HP unplugged),
// true = Headphone EQ (HP plugged). Set by dsp_set_hp_plugged(); s_hp_dirty
// forces a coeff recompute + delay-line reset on the next block even though the
// settings generation didn't change.
static volatile bool s_hp_plugged = false;
static volatile bool s_hp_dirty   = false;
static bool     s_eq_on;                 // active local EQ enabled (speaker or HP)
static bool     s_bt_eq_on;              // bluetooth EQ enabled
static float    s_vol_target = 0.0f;     // speaker volume gain target
static float    s_cur_gain = 0.0f;       // smoothed speaker volume (starts muted)
static float    s_bt_vol_target = 1.0f;  // bluetooth volume gain target
static float    s_bt_cur_gain = 1.0f;    // smoothed bluetooth volume
static float    s_sfx_gain = 0.0f;       // linear cue-mix gain (0 = sfx disabled)

static inline int16_t sat16(float x)
{
    if (x > 32767.0f)  x = 32767.0f;
    if (x < -32768.0f) x = -32768.0f;
    return (int16_t)lrintf(x);
}

static float db_to_lin(float db) { return powf(10.0f, db / 20.0f); }

// esp-dsp's dsps_biquad_gen_peakingEQ_f32 takes no gain argument, so the
// RBJ-cookbook peaking coefficients for the mid band are computed by hand
// (esp-dsp still runs the filter via dsps_biquad_f32). f is normalized (Fc/Fs);
// output coeffs = {b0,b1,b2,a1,a2} normalized so a0 = 1.
static void gen_peaking(float *c, float f, float gain_db, float q)
{
    float A     = powf(10.0f, gain_db / 40.0f);
    float w0    = 2.0f * (float)M_PI * f;
    float cw    = cosf(w0);
    float alpha = sinf(w0) / (2.0f * q);
    float a0    = 1.0f + alpha / A;
    c[0] = (1.0f + alpha * A) / a0;   // b0
    c[1] = (-2.0f * cw)       / a0;   // b1
    c[2] = (1.0f - alpha * A) / a0;   // b2
    c[3] = (-2.0f * cw)       / a0;   // a1
    c[4] = (1.0f - alpha / A) / a0;   // a2
}

// Recompute everything that depends on settings, only when the generation
// counter changes (coefficient generation is the expensive part). Both process
// functions call this at the top of a block; the first one does the work, the
// second sees an unchanged generation and skips. Keeps a single consistent
// snapshot per block across the speaker and BT paths.
static void maybe_recompute(void)
{
    uint32_t gen = settings_generation();
    bool hp_switch = s_hp_dirty;
    if (gen == s_last_gen && !hp_switch) return;
    s_hp_dirty = false;

    gbhifi_settings_t s;
    settings_get(&s);

    // Local-path EQ: pick the profile by which output is live (the DAC drives
    // only one; the speaker amp is muted when HP is plugged), then build the band
    // coefficients from that profile's gains. Speaker EQ voices the small GBA
    // driver; Headphone EQ is independent (flat by default).
    bool   l_on     = s_hp_plugged ? s.eq_hp_enabled   : s.eq_enabled;
    int8_t l_bass   = s_hp_plugged ? s.eq_hp_bass_db   : s.eq_bass_db;
    int8_t l_mid    = s_hp_plugged ? s.eq_hp_mid_db    : s.eq_mid_db;
    int8_t l_treble = s_hp_plugged ? s.eq_hp_treble_db : s.eq_treble_db;
    dsps_biquad_gen_lowShelf_f32 (s_coef_bass,   F_BASS,   (float)l_bass,   Q_SHELF);
    gen_peaking                  (s_coef_mid,    F_MID,    (float)l_mid,    Q_PEAK);
    dsps_biquad_gen_highShelf_f32(s_coef_treble, F_TREBLE, (float)l_treble, Q_SHELF);
    s_eq_on = l_on;

    // On a profile swap (HP plug/unplug) the delay lines hold the old filter's
    // state; flush them so the new curve doesn't ring. A settings-only change
    // (volume/EQ tweak) leaves them alone to avoid a click on every edit.
    if (hp_switch) {
        memset(s_w_bass,   0, sizeof(s_w_bass));
        memset(s_w_mid,    0, sizeof(s_w_mid));
        memset(s_w_treble, 0, sizeof(s_w_treble));
    }

    // Bluetooth EQ coefficients (same bands, independent gains).
    dsps_biquad_gen_lowShelf_f32 (s_bt_coef_bass,   F_BASS,   (float)s.eq_bt_bass_db,   Q_SHELF);
    gen_peaking                  (s_bt_coef_mid,    F_MID,    (float)s.eq_bt_mid_db,    Q_PEAK);
    dsps_biquad_gen_highShelf_f32(s_bt_coef_treble, F_TREBLE, (float)s.eq_bt_treble_db, Q_SHELF);
    s_bt_eq_on = s.eq_bt_enabled;

    float v   = (float)s.speaker_vol_pct / 100.0f;
    float bv  = (float)s.bt_vol_pct      / 100.0f;
    s_vol_target    = v * v;
    s_bt_vol_target = bv * bv;
    s_sfx_gain      = s.sfx_enabled ? db_to_lin((float)s.sfx_level_db) : 0.0f;

    s_last_gen = gen;
    ESP_LOGI(TAG, "recompute: spkVol=%u%% btVol=%u%% localEQ=%s:%d[%d/%d/%d] btEQ=%d[%d/%d/%d] sfx=%d",
             s.speaker_vol_pct, s.bt_vol_pct,
             s_hp_plugged ? "HP" : "spk", l_on, l_bass, l_mid, l_treble,
             s.eq_bt_enabled, s.eq_bt_bass_db, s.eq_bt_mid_db, s.eq_bt_treble_db,
             s.sfx_enabled);
}

esp_err_t dsp_init(void)
{
    maybe_recompute();
    // Seed the smoothed gains at their targets so the very first blocks aren't
    // ramping up from silence audibly.
    s_cur_gain    = s_vol_target;
    s_bt_cur_gain = s_bt_vol_target;
    ESP_LOGI(TAG, "dsp ready");
    return ESP_OK;
}

void dsp_set_hp_plugged(bool plugged)
{
    // Only flag a profile swap on an actual change. The audio task picks this up
    // in maybe_recompute() on its next block (rebuilds the local-EQ coeffs from
    // the HP-vs-speaker gains and flushes the delay lines). Plain bool writes are
    // atomic on ESP32, no lock needed.
    if (plugged != s_hp_plugged) {
        s_hp_plugged = plugged;
        s_hp_dirty   = true;
    }
}

void dsp_process_local(int16_t *stereo, size_t frames)
{
    if (!stereo || frames == 0) return;
    if (frames > MAX_BLOCK) frames = MAX_BLOCK;
    maybe_recompute();

    // Deinterleave L/R/L/R into two float channels (esp-dsp biquads need
    // contiguous buffers). HP gets these as true stereo; the speaker sums them
    // passively.
    static float l[MAX_BLOCK], r[MAX_BLOCK];
    for (size_t i = 0; i < frames; i++) {
        l[i] = (float)stereo[i * 2];
        r[i] = (float)stereo[i * 2 + 1];
    }

    // Speaker EQ per channel (each biquad keeps its own per-channel delay line).
    if (s_eq_on) {
        dsps_biquad_f32(l, l, frames, s_coef_bass,   s_w_bass[0]);
        dsps_biquad_f32(l, l, frames, s_coef_mid,    s_w_mid[0]);
        dsps_biquad_f32(l, l, frames, s_coef_treble, s_w_treble[0]);
        dsps_biquad_f32(r, r, frames, s_coef_bass,   s_w_bass[1]);
        dsps_biquad_f32(r, r, frames, s_coef_mid,    s_w_mid[1]);
        dsps_biquad_f32(r, r, frames, s_coef_treble, s_w_treble[1]);
    }

    // Digital volume (perceptual squared curve, smoothed once per frame so both
    // channels share the same gain) applied to the program only, then the shared
    // SFX cue mix (mono cue into both channels, post-volume, fixed level so cues
    // stay audible regardless of volume). The cue was rendered once for this block
    // by sfx_generate_block(); the BT path mixes the same samples.
    const float *cue = (s_sfx_gain > 0.0f && sfx_cue_active()) ? sfx_cue() : NULL;
    for (size_t i = 0; i < frames; i++) {
        s_cur_gain += (s_vol_target - s_cur_gain) * VOL_SMOOTH;
        float c = cue ? s_sfx_gain * cue[i] : 0.0f;
        stereo[i * 2]     = sat16(l[i] * s_cur_gain + c);
        stereo[i * 2 + 1] = sat16(r[i] * s_cur_gain + c);
    }
}

void dsp_process_bt(int16_t *stereo, size_t frames)
{
    if (!stereo || frames == 0) return;
    if (frames > MAX_BLOCK) frames = MAX_BLOCK;
    maybe_recompute();

    // Decide whether the BT stream needs any processing this block. When BT EQ is
    // off, the volume is at unity (and the smoothed gain has settled there), and
    // no cue is playing, the stream passes through untouched (full fidelity,
    // cheap). This is the common default case.
    bool cue       = (s_sfx_gain > 0.0f) && sfx_cue_active();
    bool vol_active = (s_bt_vol_target < 0.999f) ||
                      (fabsf(s_bt_cur_gain - 1.0f) > 0.0005f);
    if (!s_bt_eq_on && !vol_active && !cue) {
        s_bt_cur_gain = s_bt_vol_target;   // keep settled at unity, no audio touched
        return;
    }

    // Deinterleave L/R/L/R into two float channels. esp-dsp biquads operate on
    // contiguous buffers, so they can't run strided.
    static float l[MAX_BLOCK], r[MAX_BLOCK];
    for (size_t i = 0; i < frames; i++) {
        l[i] = (float)stereo[i * 2];
        r[i] = (float)stereo[i * 2 + 1];
    }

    // EQ (per channel, each biquad keeps its own delay line).
    if (s_bt_eq_on) {
        dsps_biquad_f32(l, l, frames, s_bt_coef_bass,   s_btw_bass[0]);
        dsps_biquad_f32(l, l, frames, s_bt_coef_mid,    s_btw_mid[0]);
        dsps_biquad_f32(l, l, frames, s_bt_coef_treble, s_btw_treble[0]);
        dsps_biquad_f32(r, r, frames, s_bt_coef_bass,   s_btw_bass[1]);
        dsps_biquad_f32(r, r, frames, s_bt_coef_mid,    s_btw_mid[1]);
        dsps_biquad_f32(r, r, frames, s_bt_coef_treble, s_btw_treble[1]);
    }

    // Digital volume (smoothed per sample, click-free) then the shared SFX cue
    // mix (post-volume, fixed level, same samples the speaker path mixes).
    const float *cue_buf = cue ? sfx_cue() : NULL;
    for (size_t i = 0; i < frames; i++) {
        s_bt_cur_gain += (s_bt_vol_target - s_bt_cur_gain) * VOL_SMOOTH;
        float c = cue_buf ? s_sfx_gain * cue_buf[i] : 0.0f;
        stereo[i * 2]     = sat16(l[i] * s_bt_cur_gain + c);
        stereo[i * 2 + 1] = sat16(r[i] * s_bt_cur_gain + c);
    }
}

