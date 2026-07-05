#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Local-path DSP for the ES8388 DAC fan-out. Runs stereo: the DAC feeds the
// stereo HP amp directly, and the mono speaker is summed passively by the 2-R
// L+R network on LOUT2/ROUT2, so the DAC stream must stay stereo to keep HP
// stereo. The BT/A2DP path has its own independent processing (dsp_process_bt).
//
// Chain (per block, per channel): speaker EQ biquads, smoothed digital volume,
// SFX cue mix (mono cue into both channels), saturate back to int16. All
// parameters come from the settings store; the engine recomputes EQ coefficients
// only when settings_generation() changes.

esp_err_t dsp_init(void);

// Tell the local-path EQ which output is live so it can pick the right profile:
// false = speaker (HP unplugged) uses Speaker EQ gains; true = wired headphones
// uses Headphone EQ gains. The DAC drives only one at a time (the speaker amp is
// muted when HP is in), so one biquad chain serves both: this swaps which gain
// set its coefficients come from and resets the chain's delay lines so the
// profile change is click-free. Call on the HP-detect edge and at boot.
void dsp_set_hp_plugged(bool plugged);

// Arm the boot startup-chime intro: mute the live GBA passthrough on the speaker
// path until the startup clip finishes (or never starts), so the mod's clean full
// chime plays alone instead of doubling with the truncated live chime, and the
// chime follows the volume wheel. Call once, before audio_pipeline_start(), only
// when the startup clip is actually going to play. Not arming it leaves the
// passthrough live -- which is exactly the future "let the GBA's own chime play"
// path. No-op on normal operation; self-releases when the clip ends.
void dsp_begin_intro(void);

// Register a callback fired (from the audio task) when the noise gate transitions
// into or out of deep silence, so app_sm can mute/unmute the speaker amp in step
// with the gate (killing the Class-D idle noise on silence). NULL to disable.
void dsp_set_gate_mute_cb(void (*cb)(bool muting));

// Process `frames` interleaved stereo int16 samples (L,R,L,R) in place. Called
// from the audio pipeline task on the local DAC fan-out buffer.
void dsp_process_local(int16_t *stereo, size_t frames);

// Optional source-side EQ on the Bluetooth/A2DP path: `frames` interleaved
// stereo int16 samples (L,R,L,R) processed in place. Independent of the speaker
// EQ and disabled by default: a no-op (leaves the buffer flat) unless the user
// enables BT EQ. Called on the BT fan-out buffer before it is pushed to the SBC
// encoder's stream buffer.
void dsp_process_bt(int16_t *stereo, size_t frames);

