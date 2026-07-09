#include "audio_pipeline.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "i2s_codec.h"
#include "dsp.h"
#include "sfx.h"

static const char *TAG = "audio";

// 256 stereo frames x 8 bytes/frame = 2 KB per read, ~5.8 ms at 44.1 kHz.
// Frequent small reads keep the producer/consumer relationship tight at the cost
// of more task wakeups per second.
#define READ_FRAMES 256
#define FRAME_BYTES 8                       // I2S input: stereo, 32-bit slots
#define OUT_FRAME_BYTES 4                   // PCM output: stereo, 16-bit

// At 44.1 kHz stereo 16-bit, audio is 176.4 KB/s (~176 bytes/ms). The stream
// buffer absorbs the BT consumer's burst pull pattern (it idles while L2CAP TX
// completes, then grabs a chunk). Sized to hold the ~5 KB natural burst swing
// (observed fill range 1-6 KB) plus a little margin above the high-water mark.
#define PCM_STREAM_BUFFER_BYTES (8 * 1024)     // ~46 ms max; was 12 KB
// High-water trim: the producer skips its push while the queue is already at/above
// this level, so latency stays bounded (~34 ms) instead of drifting toward full as
// the 44.1 kHz producer slowly outruns the sink's consume rate (no rate matching).
// Set above the ~5 KB burst swing so normal bursts aren't clipped -- only the slow
// drift trips it, dropping one ~6 ms block occasionally (rare, ~inaudible) rather
// than letting the fill climb to a 70 ms overflow.
#define PCM_STREAM_HIGH_WATER   (6 * 1024)
// Wake the BT consumer as soon as one SBC-sized block is ready.
// 512 bytes = 128 stereo frames ~2.9 ms.
#define PCM_STREAM_TRIGGER_LEVEL 512

#define MAG_LOG_INTERVAL_US 1000000          // peak log once per second

// On-demand ADC capture for noise-floor / spectrum analysis. audio_pipeline_capture()
// requests N raw samples; the pipeline grabs the next N frames verbatim (no arming, so
// it captures the quiet noise floor, not just loud audio) and dumps them as CSV between
// WAVEDUMP_BEGIN / WAVEDUMP_END markers over UART for tools/plot_wavedump.py. The dump
// printf stalls this real-time task ~1 s, so audio hiccups during a capture (fine for a
// bench measurement; the samples are already grabbed before the dump runs).
#define WAVEDUMP_MAX 1024                    // 23.2 ms at 44.1 kHz
static volatile int s_wd_request = 0;        // samples to capture on next read (0 = idle)

static StreamBufferHandle_t s_pcm_stream = NULL;
// True only while a sink is actively streaming (set from bt_a2d on the media
// state). Gates the producer push so the buffer isn't pre-filled to full while
// idle/connecting. Written from the BT task, read by the pipeline task; a plain
// bool write is atomic on ESP32.
static volatile bool s_bt_streaming = false;

// Prebuffer: at stream start the consumer emits silence until this many bytes have
// queued, so real audio only flows once there's a cushion. Filling from empty
// otherwise underruns for the first ~0.5 s -- each partial read is real samples
// then zero-pad, a discontinuity the SBC encoder renders as garble. ~23 ms at
// 44.1 kHz/16-bit/stereo; small enough to stay low-latency, large enough to cover
// the consumer's bursty pull. Cleared on stop so each connect re-primes.
#define PCM_PRIME_TARGET_BYTES  (4 * 1024)
static volatile bool s_pcm_primed = false;

// Cooperative park (Mode A: stop the digital path so I2S/MCLK can stop).
// audio_pipeline_stop() sets s_park_req; the task acks via s_parked_sem at the
// top of its loop and blocks on a task notification until audio_pipeline_resume()
// wakes it. The task stays alive, so silence/peak window state and buffers
// persist, and I2S is never torn down under an in-flight read.
static TaskHandle_t      s_pipeline_task = NULL;
static SemaphoreHandle_t s_parked_sem    = NULL;
static volatile bool     s_park_req       = false;

// Silence detection state. Threshold is the int32 sample value: the ADC delivers
// 24-in-32 left-justified, so the int32 sample = 24-bit value << 8.
#define SILENCE_THRESHOLD_I32   0x10000
// Consecutive ~1 s below-threshold windows before declaring silence.
#define SILENCE_WINDOWS_TO_QUIET CONFIG_GBHIFI_SILENCE_WINDOWS

static audio_silence_cb_t s_silence_cb = NULL;
static bool s_is_silent = false;
static int  s_silent_window_count = 0;

void audio_pipeline_set_silence_cb(audio_silence_cb_t cb)
{
    s_silence_cb = cb;
}

void audio_pipeline_capture(int samples)
{
    if (samples <= 0 || samples > WAVEDUMP_MAX) samples = WAVEDUMP_MAX;
    s_wd_request = samples;
}

void audio_pipeline_set_bt_streaming(bool streaming)
{
    s_bt_streaming = streaming;
    if (!streaming) s_pcm_primed = false;   // re-prebuffer on the next connect
}

bool audio_pipeline_is_silent(void)
{
    return s_is_silent;
}

static void update_silence(int32_t peak_max)
{
    bool below = (peak_max < SILENCE_THRESHOLD_I32);
    if (below) {
        if (s_silent_window_count < SILENCE_WINDOWS_TO_QUIET) {
            s_silent_window_count++;
        }
        if (!s_is_silent && s_silent_window_count >= SILENCE_WINDOWS_TO_QUIET) {
            s_is_silent = true;
            if (s_silence_cb) s_silence_cb(true);
        }
    } else {
        s_silent_window_count = 0;
        if (s_is_silent) {
            s_is_silent = false;
            if (s_silence_cb) s_silence_cb(false);
        }
    }
}

static void pipeline_task(void *arg)
{
    // The ES8388 ADC emits 24-bit samples in 32-bit I2S slots (32-in-32), so
    // each int32 holds the 24-bit sample value in bits 31:8, zero-pad in 7:0. A
    // sign-extending right-shift by 16 gives a 16-bit signed value, discarding
    // the 8 least-significant bits of resolution.
    static int32_t stereo_buf[READ_FRAMES * 2];     // ADC in: int32 L,R
    static int16_t out_buf[READ_FRAMES * 2];        // BT PCM out: int16 L,R interleaved
    static int16_t local_buf[READ_FRAMES * 2];      // local DAC program: int16 L,R interleaved (stereo)
    static int32_t dac_buf[READ_FRAMES * 2];        // DAC out: int32 L,R (stereo)

    int32_t peak_l = 0, peak_r = 0;
    size_t  fill_min = SIZE_MAX, fill_max = 0;
    int64_t window_start_us = esp_timer_get_time();
    // On-demand capture buffer (filled across reads, one read is only READ_FRAMES).
    static int32_t wd_buf[WAVEDUMP_MAX * 2];
    int wd_have = 0;      // samples captured into the active dump
    int wd_target = 0;    // 0 = not capturing; else samples requested for this dump
    bool bt_streaming_prev = false;   // edge-detect the streaming gate for buffer reset

    while (1) {
        // Cooperative park for Mode A: when stop() requests it, ack and block
        // until resume() notifies us. Done at the loop top so we are not inside
        // i2s_codec_read when the channel is disabled. On wake, I2S has already
        // been restarted by resume(), so fall through to the next read.
        if (s_park_req) {
            s_park_req = false;
            xSemaphoreGive(s_parked_sem);
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        size_t bytes_read = 0;
        esp_err_t err = i2s_codec_read(stereo_buf, sizeof(stereo_buf), &bytes_read, 100);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2s read: %s (bytes=%u)",
                     esp_err_to_name(err), (unsigned)bytes_read);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        size_t frames = bytes_read / FRAME_BYTES;

        // On-demand capture: when a request lands, grab the next wd_target frames
        // verbatim (no arming, so the quiet noise floor is captured as-is), then dump
        // as CSV between markers. printf (not ESP_LOG) so the host parser gets clean
        // lines. The dump stalls this task ~1 s; audio hiccups then recovers.
        if (wd_target == 0 && s_wd_request > 0) {
            wd_target = s_wd_request;
            s_wd_request = 0;
            wd_have = 0;
        }
        if (wd_target > 0) {
            for (size_t i = 0; i < frames && wd_have < wd_target; i++) {
                wd_buf[wd_have * 2]     = stereo_buf[i * 2];
                wd_buf[wd_have * 2 + 1] = stereo_buf[i * 2 + 1];
                wd_have++;
            }
            if (wd_have >= wd_target) {
                printf("WAVEDUMP_BEGIN n=%d fs=44100 fmt=24in32\n", wd_target);
                for (int i = 0; i < wd_target; i++) {
                    printf("%ld,%ld\n",
                           (long)wd_buf[i * 2], (long)wd_buf[i * 2 + 1]);
                }
                printf("WAVEDUMP_END\n");
                wd_target = 0;
            }
        }

        // Convert stereo 32-bit slot data to 16-bit interleaved stereo, tracking
        // peaks for the once-per-second magnitude log. The sign-extending
        // right-shift by 16 keeps the 16 most significant bits of the 24-bit
        // sample, discarding the lowest 8 bits of resolution.
        for (size_t i = 0; i < frames; i++) {
            int32_t l = stereo_buf[i * 2];
            int32_t r = stereo_buf[i * 2 + 1];

            int32_t la = (l < 0) ? -l : l;
            int32_t ra = (r < 0) ? -r : r;
            if (la > peak_l) peak_l = la;
            if (ra > peak_r) peak_r = ra;

            int16_t lo = (int16_t)(l >> 16);
            int16_t ro = (int16_t)(r >> 16);
            out_buf[i * 2]     = lo;
            out_buf[i * 2 + 1] = ro;
            // Local program stays stereo. The ES8388 DAC drives the HP amp
            // (LOUT1/ROUT1) directly, which must stay stereo, and the line out
            // (LOUT2/ROUT2) feeds a passive 2-R L+R sum to the mono speaker amp.
            // The speaker mono-down happens in hardware, not here.
            local_buf[i * 2]     = lo;
            local_buf[i * 2 + 1] = ro;
        }

        // Render this block's SFX cue once (advances cue state a single time) so
        // the same samples can be mixed into both the BT and DAC paths.
        sfx_generate_block(frames);

        // BT DSP: source-side noise reduction (HPF/LPF/notch + gate) + optional EQ
        // + digital volume + SFX cue mix, in place on out_buf before the
        // stream-buffer push so the SBC encoder sees the cleaned, gated signal (not
        // the raw capture floor, which SBC renders as audible digital hash). No-op
        // passthrough only when nr is off AND BT EQ is off, volume is unity, and no
        // cue is playing.
        dsp_process_bt(out_buf, frames);

        // Push the interleaved stereo PCM into the stream buffer for A2DP, but only
        // while a sink is actively streaming. Gating the fill keeps the buffer from
        // pre-filling to full while idle/connecting -- which otherwise hands the SBC
        // encoder a full (~70 ms) buffer at connect that overflows and drops for a
        // second or two. On the streaming->idle edge, empty the buffer so the next
        // connect starts from ~0 latency; the consumer (a2d_data_cb) is idle once the
        // media stops, so the reset does not race a concurrent read. Non-blocking
        // send: if the consumer falls behind, drop rather than stall I2S DMA.
        bool bt_streaming = s_bt_streaming;
        if (s_pcm_stream && bt_streaming) {
            // High-water trim: skip the push while the queue is already at the mark,
            // so latency stays bounded instead of drifting toward full. Drops the
            // newest ~6 ms block once per drift accumulation (rare) rather than
            // letting the fill climb to an eventual overflow.
            if (xStreamBufferBytesAvailable(s_pcm_stream) < PCM_STREAM_HIGH_WATER) {
                (void)xStreamBufferSend(s_pcm_stream, out_buf,
                                        frames * OUT_FRAME_BYTES, 0);
            }
            size_t fill = xStreamBufferBytesAvailable(s_pcm_stream);
            if (fill < fill_min) fill_min = fill;
            if (fill > fill_max) fill_max = fill;
        } else if (s_pcm_stream && bt_streaming_prev && !bt_streaming) {
            xStreamBufferReset(s_pcm_stream);   // streaming ended: drop stale audio
        }
        bt_streaming_prev = bt_streaming;

        // Local DSP: stereo speaker EQ, digital volume, SFX cue mix, saturate,
        // in place on the interleaved local buffer.
        dsp_process_local(local_buf, frames);

        // Fan the processed program out to the ES8388 DAC in stereo (sample value
        // in bits 31:8 to match the ADC framing). The DAC drives the HP amp (true
        // stereo) plus the line out to the passive L+R sum and mono speaker amp,
        // which the state machine's mute policy gates (silent during BT streaming
        // and while wired HP is plugged). Same clock domain as the RX read, so
        // writing the same frame count keeps the bus balanced.
        for (size_t i = 0; i < frames; i++) {
            dac_buf[i * 2]     = (int32_t)local_buf[i * 2]     << 16;
            dac_buf[i * 2 + 1] = (int32_t)local_buf[i * 2 + 1] << 16;
        }
        size_t dac_written = 0;
        (void)i2s_codec_write(dac_buf, frames * FRAME_BYTES, &dac_written, 100);

        int64_t now_us = esp_timer_get_time();
        if (now_us - window_start_us >= MAG_LOG_INTERVAL_US) {
#if CONFIG_GBHIFI_DEBUG_PEAK_LOG
            // Opt-in per-second ADC level readout for gain/clip checks. Default
            // off (ESP_LOGD), compiled out at the INFO log level.
            ESP_LOGI(TAG, "peak L=0x%08lx R=0x%08lx  buf=%u-%u/%u B",
#else
            ESP_LOGD(TAG, "peak L=0x%08lx R=0x%08lx  buf=%u-%u/%u B",
#endif
                     (unsigned long)peak_l, (unsigned long)peak_r,
                     (unsigned)fill_min, (unsigned)fill_max,
                     (unsigned)PCM_STREAM_BUFFER_BYTES);
            update_silence((peak_l > peak_r) ? peak_l : peak_r);
            peak_l = 0;
            peak_r = 0;
            fill_min = SIZE_MAX;
            fill_max = 0;
            window_start_us = now_us;
        }
    }
}

esp_err_t audio_pipeline_start(void)
{
    if (!s_pcm_stream) {
        s_pcm_stream = xStreamBufferCreate(PCM_STREAM_BUFFER_BYTES,
                                           PCM_STREAM_TRIGGER_LEVEL);
        if (!s_pcm_stream) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_parked_sem) {
        s_parked_sem = xSemaphoreCreateBinary();
        if (!s_parked_sem) {
            return ESP_ERR_NO_MEM;
        }
    }

    // 4 KB stack: the large buffers are static, so the task mainly needs space
    // for the i2s driver call chain and printf inside the wavedump block.
    // Priority 10 to preempt user-priority tasks and keep the producer/consumer
    // hand-off tight.
    //
    // Pinned to core 1 (APP_CPU): the BT controller + Bluedroid are pinned to
    // core 0. During A2DP streaming that BT work would otherwise preempt this
    // producer for 20-26 ms at a time on a shared core, underrunning the A2DP
    // stream and garbling BT. The other core isolates the audio producer from BT
    // preemption.
    BaseType_t ok = xTaskCreatePinnedToCore(pipeline_task, "audio_pipe",
                                            4096, NULL, 10, &s_pipeline_task, 1);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}

esp_err_t audio_pipeline_stop(void)
{
    if (!s_pipeline_task || !s_parked_sem) return ESP_ERR_INVALID_STATE;
    // Ask the task to park, then wait for its ack before stopping I2S so the
    // channel is never disabled under an in-flight i2s_channel_read. The read
    // timeout is 100 ms, so the task parks within one read; allow generous slack.
    s_park_req = true;
    if (xSemaphoreTake(s_parked_sem, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGW(TAG, "pipeline did not park in time");
        s_park_req = false;
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = i2s_codec_stop();
    ESP_LOGI(TAG, "pipeline stopped (I2S off): %s", esp_err_to_name(err));
    return err;
}

esp_err_t audio_pipeline_resume(void)
{
    if (!s_pipeline_task) return ESP_ERR_INVALID_STATE;
    // Fully rebuild the I2S bus before un-parking the task. A bare
    // i2s_codec_start() (re-enable) is not enough after audio_pipeline_stop():
    // when Mode A light-sleeps the CPU, the APLL that feeds MCLK/BCLK is powered
    // down, and re-enabling the channels against it yields wrong clocks. The ADC
    // read then returns un-paced (the task spins, starving the core-1 idle task
    // and tripping the task-WDT) and the audio is garbled. Delete + recreate
    // re-locks the APLL from scratch. The ES8388 keeps its registers across this
    // (separately powered), so no re-config is needed; the caller re-asserts DSP
    // mode after.
    i2s_codec_deinit();
    esp_err_t err = i2s_codec_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s reinit failed: %s", esp_err_to_name(err));
        return err;
    }
    err = i2s_codec_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s restart failed: %s", esp_err_to_name(err));
        return err;
    }
    xTaskNotifyGive(s_pipeline_task);
    ESP_LOGI(TAG, "pipeline resumed (I2S rebuilt)");
    return ESP_OK;
}

size_t audio_pipeline_read_stereo16(void *dst, size_t max_bytes,
                                    uint32_t timeout_ms)
{
    if (!s_pcm_stream || !dst || max_bytes == 0) {
        return 0;
    }
    // Prebuffer at stream start: hold real audio back until a cushion has built. A
    // 0-length return makes the A2DP callback emit clean silence; once the queue
    // reaches the target we latch primed and stream normally. Re-primes on the next
    // connect (the gate clears s_pcm_primed on stop).
    if (!s_pcm_primed) {
        if (xStreamBufferBytesAvailable(s_pcm_stream) < PCM_PRIME_TARGET_BYTES) {
            return 0;
        }
        s_pcm_primed = true;
    }
    return xStreamBufferReceive(s_pcm_stream, dst, max_bytes,
                                pdMS_TO_TICKS(timeout_ms));
}
