#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// Runtime-tunable DSP parameters.
//
// Single source of truth for everything a control surface (serial console, web
// UI) can change. Those surfaces only call the settings_* API here; they never
// touch the audio path. The DSP engine (dsp.c) is a pure consumer: it takes a
// snapshot per block and recomputes filter coefficients only when `generation`
// changes.
//
// Persisted as a single versioned blob in NVS (namespace "gbhifi_cfg", key
// "dsp"). Bump GBHIFI_SETTINGS_VERSION when the struct layout changes; an
// older/absent blob falls back to the Kconfig-seeded defaults.

#define GBHIFI_SETTINGS_VERSION 6

typedef struct {
    uint16_t version;          // == GBHIFI_SETTINGS_VERSION
    uint8_t  speaker_vol_pct;  // local-speaker digital volume, 0..100
    uint8_t  bt_vol_pct;       // Bluetooth digital volume, 0..100 (100 = unity)

    // Speaker EQ (local DAC path while no headphones are plugged). Voices the
    // small GBA driver (bass roll-off + presence). 3 bands, gains in dB, -12..+12.
    bool     eq_enabled;
    int8_t   eq_bass_db;       // low-shelf  ~150 Hz
    int8_t   eq_mid_db;        // peak       ~2.5 kHz
    int8_t   eq_treble_db;     // high-shelf ~5 kHz

    // Headphone EQ (same local DAC path, but the profile used while wired
    // headphones are plugged). One DSP chain feeds the ES8388 DAC, which drives
    // either the speaker (HP out) or the HP amp (HP in), never both at once
    // (pam_apply mutes the speaker when HP is plugged), so app_sm swaps which
    // gain set the EQ uses on the HP-detect edge (dsp_set_hp_plugged). Off by
    // default with flat gains. Same 3 bands/freqs.
    bool     eq_hp_enabled;
    int8_t   eq_hp_bass_db;
    int8_t   eq_hp_mid_db;
    int8_t   eq_hp_treble_db;

    // Bluetooth EQ (A2DP path). Off by default; an optional source-side tone
    // control. Same 3 bands/freqs as the speaker, independent gains. Applied in
    // stereo before the SBC encoder.
    bool     eq_bt_enabled;
    int8_t   eq_bt_bass_db;
    int8_t   eq_bt_mid_db;
    int8_t   eq_bt_treble_db;

    bool     sfx_enabled;      // allow cues (chimes/clips) to mix in (speaker only)
    int8_t   sfx_level_db;     // cue mix gain trim, -24..+6 dB (0 = authored)

    // ---- Operating-mode preferences ----
    // Sticky user choice of audio path. app_sm owns the actual transition; a
    // control surface (console / BLE) only flips this flag and app_sm reacts.
    //   false = Mode B (full DSP: EQ/SFX/volume + A2DP, ES8388 ADC to DAC)
    //   true  = Mode A (analog bypass: ES8388 LIN to outputs, digital path stopped)
    bool     mode_a;
    // On a wired-HP unplug while in Mode A: false = stay in Mode A (speaker in
    // battery mode); true = switch to Mode B (speaker uses full DSP).
    bool     unplug_to_b;

    // ---- R-button hold-menu thresholds (ms) ----
    // The Connect/Pair (R) button drives a chime-guided hold menu: held past
    // each threshold the device chimes that rung; release in a zone runs its
    // action. Runtime-tunable so the user can lengthen/shorten the gesture (R is
    // mashed during gameplay). Defaults seed from Kconfig. Absolute thresholds
    // from press, strictly increasing: connect <= pair <= mode. buttons.c reads
    // these; sanitise() enforces order.
    uint16_t hold_connect_ms;   // release >= here, < pair: connect bonded
    uint16_t hold_pair_ms;      // release >= here, < mode: pairing
    uint16_t hold_mode_ms;      // release >= here:         toggle Mode A/B
    // Plain hold to leave Mode A (no menu, since the DSP/SFX path is stopped in
    // bypass so the menu chimes can't play). Timed by app_sm's light-sleep loop.
    uint16_t hold_mode_exit_ms;
} gbhifi_settings_t;

// Load the blob from NVS, or seed Kconfig defaults if absent / older version.
// Idempotent; safe to call once at boot before the audio pipeline starts.
esp_err_t settings_init(void);

// Copy the current settings under lock. Cheap; the audio task calls this once
// per ~5.8 ms block. Always returns a fully-populated, validated struct.
void settings_get(gbhifi_settings_t *out);

// Monotonic counter, bumped on every successful setter. Lets a consumer cheaply
// detect "params changed" without diffing the struct. Plain aligned 32-bit
// read, atomic on ESP32, no lock needed.
uint32_t settings_generation(void);

// Setters: validate/clamp, update the master copy under lock, bump generation.
// Do NOT auto-persist (flash wear); call settings_commit() to save.
esp_err_t settings_set_volume(uint8_t pct);
esp_err_t settings_set_bt_volume(uint8_t pct);
esp_err_t settings_set_eq(bool enabled, int8_t bass_db, int8_t mid_db, int8_t treble_db);
esp_err_t settings_set_hp_eq(bool enabled, int8_t bass_db, int8_t mid_db, int8_t treble_db);
esp_err_t settings_set_bt_eq(bool enabled, int8_t bass_db, int8_t mid_db, int8_t treble_db);
esp_err_t settings_set_sfx(bool enabled, int8_t level_db);
esp_err_t settings_set_mode_a(bool mode_a);
esp_err_t settings_set_unplug_to_b(bool unplug_to_b);
// Set the R-button hold-menu thresholds (ms). Clamped + re-ordered so
// connect <= pair <= mode (sanitise()). mode_exit is independent.
esp_err_t settings_set_hold_timings(uint16_t connect_ms, uint16_t pair_ms,
                                    uint16_t mode_ms, uint16_t mode_exit_ms);

// Persist the current settings to NVS. Explicit (not per-set) to bound wear.
esp_err_t settings_commit(void);

