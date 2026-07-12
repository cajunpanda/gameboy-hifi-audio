#pragma once

#include <stdbool.h>

#include "esp_err.h"

// Top-level state machine. Owns transitions between DEEP_IDLE, STANDBY,
// PAIRING, CONNECTED_IDLE, and STREAMING. Subscribes to buttons, bt_a2d, and
// audio_pipeline events and drives connection attempts, pairing entry, A2DP
// media start/suspend, and the speaker amp mute via PIN_PAM_SD.
//
// Call after buttons_init / audio_pipeline_start / bt_a2d_init have all
// returned ESP_OK. Boots directly into STANDBY.
esp_err_t app_sm_start(void);

// Bring the local-speaker amp up early, before the BT stack init + app_sm_start()
// that would otherwise gate it seconds out. Samples HP-detect and applies the same
// HP-aware rule as steady state: speaker amp on only when no headphones are plugged
// (the ES8388 DAC feeds both outputs, so with HP in the speaker must stay muted).
// Lets the Game Boy power-on chime play the earliest possible moment without
// clipping. Idempotent with the pam_init() inside app_sm_start(), which re-evaluates
// the amp against the live HP/gate/STREAMING state.
void app_sm_speaker_early_on(void);

// Bring up the VOL-wheel ADC and seed the speaker volume from the wheel before the
// audio pipeline produces its first block, so boot audio starts at the wheel
// setting rather than the stored default (which otherwise blares for a wheel-down
// user until app_sm_start()'s poll — now behind the BT hold-off — first runs).
// Call after settings_init() and before dsp_init(). Idempotent with the ADC init
// inside app_sm_start().
void app_sm_prime_volume(void);

// The HP-detect state sampled by app_sm_speaker_early_on() (true = headphones
// plugged). Lets boot bring-up seed the DSP's HP-vs-speaker EQ profile before the
// first audio block, so the startup chime is voiced for the live output. Valid only
// after app_sm_speaker_early_on(); sm_task re-samples the live pin once it runs.
bool app_sm_hp_plugged(void);

// Boot-time radio-inrush mute window. `quiet=true` forces the speaker amp SD low
// regardless of policy; `quiet=false` re-applies the normal HP/state rule. Used by
// main.c around the BT controller enable: the radio's PHY-calibration current
// spike plus a loud speaker peak can sag the boost rail below the brownout
// threshold on a low battery, so the amp is parked for that window. Only
// meaningful between app_sm_speaker_early_on() and app_sm_start() (later, the
// state machine owns the pin and re-applies policy on its own events).
void app_sm_amp_radio_quiet(bool quiet);

// Bench/console: post the same state-machine events a Connect/Pair button hold
// releases (connect zone / pair zone), so BT reconnect and pairing can be driven
// from the serial or BLE console without touching the physical button. No-ops
// (with a warning) before app_sm_start(). The state machine applies its normal
// per-state rules, so e.g. a connect request in PAIRING is ignored just as a
// button release would be.
void app_sm_request_bt_connect(void);
void app_sm_request_bt_pair(void);

// Test/debug: force an immediate transition to DEEP_IDLE (ESP32 deep sleep) from
// any awake state except PAIRING. Used by the `sleep` console command for
// deep-sleep/wake reliability cycling. Wake sources are configured exactly as in
// a normal DEEP_IDLE entry: R-button (EXT1, held) or an HP-detect edge (EXT0).
void app_sm_request_sleep(void);

// Switch operating mode by rebooting into it (Mode A boots BT-less for power +
// reliability; Mode B boots with the radio). Persists the mode, then esp_restart().
// Does not return. No-op if already in the requested mode.
void app_sm_switch_mode(bool to_mode_a);

// Hand app_sm the boot-mode decision main.c already made (so it drops into the
// same mode it skipped/kept BT init for). Call before app_sm_start().
void app_sm_set_boot_into_mode_a(bool v);

// Enable/disable the VOL-wheel poll driving volume (GPIO39 ADC). The mode-change
// poll still runs either way; this only gates whether the wheel reading is
// applied to volume. Defaults ON: the wheel is the GBA's physical volume control.
// `wheel off` (console) disables it when the console `vol` should win, or when the
// wheel is physically disconnected. In Mode B the wheel sets the local-speaker DSP
// volume; in Mode A it sets the ES8388 output drivers.
void app_sm_set_wheel_enabled(bool enabled);

// Diagnostic: sample the VOL wheel right now. *raw_out gets the 12-bit ADC value
// (-1 if the ADC is unavailable), *pct_out gets the mapped 0..100. The wheel is
// VCC-referenced (ratiometric), so the top of travel reads below full-scale and
// drifts with the battery; use this from the `wheel` console command to
// characterise the real min/max for calibration.
void app_sm_vol_wheel_read(int *raw_out, int *pct_out);

// Debug/bench: override HP-detect so a test doesn't require physically holding
// the jack. mode: 1 = force plugged, 0 = force unplugged, -1 = follow the real
// GPIO again. While forced, real jack edges are ignored. Driven by the
// `hp plug|unplug|follow` console command.
void app_sm_force_hp(int mode);

// Battery sense on ADC1_CH0 (VBAT through the R20/R21 = 100k/100k divider, so
// VBAT = 2x the sensed voltage). app_sm_read_vbat_mv returns VBAT in millivolts
// (-1 if the sense/cali is unavailable); app_sm_batt_pct maps a reading to a
// rough 0..100%; app_sm_batt_check is the periodic poll (call from the ~60 s
// heartbeat) that logs VBAT and manages the low-battery latch. The volume wheel
// is battery-referenced off the same VBAT so its calibration holds as it droops.
int  app_sm_read_vbat_mv(void);
int  app_sm_batt_pct(int vbat_mv);
void app_sm_batt_check(void);
