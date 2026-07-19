#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Cue player for the audio path. A "cue" is a short sound mixed additively on
// top of the program audio. The DSP mixes the same rendered cue into BOTH the
// local-speaker path and the A2DP/BT path (dsp_process_local + dsp_process_bt),
// so cues are audible in headphones while streaming, not just on the speaker.
// Two source kinds:
//
//   - synth: generated inline (sine + envelope), no storage. Used for the
//     pairing/connect chimes.
//   - clip:  16-bit mono PCM streamed from the LittleFS clip store
//     (/clips/<name>.gsfx). Recorded SFX and pre-rendered speech phrases;
//     the web UI uploads these.
//
// Clip files use the GSFX container (see tools/make_clip.py): 12-byte header
// {magic "GSFX", uint32 sample_rate, uint32 num_frames} then int16 LE mono PCM.
// A low-priority feeder task does all file I/O + linear resampling to the
// 44.1 kHz pipeline and pushes samples into a FIFO; sfx_render() (called from
// the audio task) only drains that FIFO + generates synth samples, so no file
// I/O ever runs on the audio task.

typedef enum {
    SFX_SYNTH_PAIRING = 0,  // entering pairing: rising attention cue
    SFX_SYNTH_CONNECT,      // sink connected: rising triad
    SFX_SYNTH_DISCONNECT,   // sink disconnected: falling two-note
    SFX_SYNTH_REBOND,       // Connect/Pair button: connect-hold confirm (rising blip)
    SFX_SYNTH_MODE,         // Mode A/B toggle: hold-menu rung + Mode A exit
    SFX_SYNTH_LOWBATT,      // battery entered the LOW band: gentle descending warning
    SFX_SYNTH_CRITBATT,     // battery entered CRITICAL: urgent repeated low triple
    SFX_SYNTH_COUNT,
} synth_id_t;

// Create the clip FIFO + request queue and start the feeder task. Call after
// fs_init() (clips are read from the mounted filesystem).
esp_err_t sfx_init(void);

// Trigger a synthesized cue. Thread-safe; latched by the audio task on its
// next block. A new trigger restarts the synth cue.
void sfx_trigger_synth(synth_id_t id);

// Trigger playback of a clip file by base name (without path or extension),
// e.g. "startup-modern" plays /clips/startup-modern.gsfx. Thread-safe; the
// feeder picks it up and preempts any clip already streaming. Silently no-ops
// if the file is missing/invalid.
void sfx_trigger_clip(const char *name);

// Render this block's cue (synth + clip) ONCE into an internal buffer,
// advancing cue state a single time. Call once per block (from the audio
// pipeline) BEFORE the per-output DSP, so the same cue samples can be mixed
// into both the BT and speaker paths. No-op work when nothing is playing.
void sfx_generate_block(size_t frames);

// Access this block's cue (int16-scale float, length = the frames passed to
// sfx_generate_block). sfx_cue_active() is false when nothing is playing, so
// callers can skip mixing entirely. The DSP applies its own cue gain.
const float *sfx_cue(void);
bool         sfx_cue_active(void);

