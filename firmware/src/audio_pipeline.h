#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Start the audio pipeline task: reads I2S from the ES8388 ADC, down-converts
// 24-in-32 stereo samples to 16-bit signed interleaved stereo (L,R,L,R), pushes
// the result into an internal stream buffer for the A2DP source data callback,
// runs the DSP (EQ/volume/SFX), and fans the processed program back out to the
// ES8388 DAC (HP amp + line out to speaker amp). Also runs the once-per-second
// magnitude logger as an "audio alive" indicator.
//
// The output format (44.1 kHz / 16-bit / stereo interleaved) is fixed by the
// ESP-IDF A2DP source endpoint's hard-coded default SBC config: it rejects
// esp_a2d_source_set_pref_mcc() and can't be reconfigured post-connect.
esp_err_t audio_pipeline_start(void);

// Park the pipeline so the I2S clocks (MCLK/BCLK/LRCK) can stop, for Mode A
// (analog bypass): the codec carries audio in hardware with no clock, so the
// digital path stops. The task cooperatively parks at the top of its loop (it is
// not killed); this blocks until the task confirms it is parked, then calls
// i2s_codec_stop(). Call only from a single owner, strictly alternating with
// audio_pipeline_resume(). Returns ESP_OK once stopped.
esp_err_t audio_pipeline_stop(void);

// Resume after audio_pipeline_stop(): restart the I2S clocks and un-park the
// task. Return the codec to its digital (Mode B) routing separately
// (es8388_set_mode) AFTER this, so the I2S DMA is being serviced again before
// the codec writes (live MCLK + unserviced DMA storms the watchdog).
esp_err_t audio_pipeline_resume(void);

// Non-blocking read of interleaved 16-bit stereo PCM bytes from the internal
// stream buffer. Returns the number of bytes actually copied (may be less than
// max_bytes if the buffer is starved). The A2DP data callback pads the remainder
// with zeros on underrun.
size_t audio_pipeline_read_stereo16(void *dst, size_t max_bytes,
                                    uint32_t timeout_ms);

// Silence detection: peak amplitude is computed per 1-second window. If the
// max(|L|, |R|) peak is below threshold for two consecutive windows, the line is
// considered silent and the callback fires with silent=true. As soon as a window
// comes in above threshold the callback fires with silent=false. Only
// state-change edges trigger the callback. Threshold is the value in the int32
// sample (24-bit shifted left 8).
typedef void (*audio_silence_cb_t)(bool silent);

void audio_pipeline_set_silence_cb(audio_silence_cb_t cb);

// Current silence state. Useful at state-machine transition entry to decide
// whether to treat audio as active, since the callback only fires on edges.
bool audio_pipeline_is_silent(void);
