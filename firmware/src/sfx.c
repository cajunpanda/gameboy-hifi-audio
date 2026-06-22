#include "sfx.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "fs.h"

static const char *TAG = "sfx";

#define PIPE_RATE        44100         // the fixed pipeline rate (audio_pipeline.c)
#define MAX_BLOCK        256           // == audio_pipeline READ_FRAMES (per-call max)

// ---- clip feeder ----------------------------------------------------------
// The feeder owns ALL file I/O + resampling. It pushes resampled int16 mono
// samples into s_clip_fifo; sfx_render drains it. A length-1 overwrite queue
// carries the latest requested clip name (newest wins / preempts).

#define CLIP_NAME_MAX    32
#define CLIP_FIFO_BYTES  (8 * 1024)    // ~93 ms at 44.1 kHz; feeder keeps it full
#define SRC_CHUNK        256           // source samples read per file read
#define CLIP_MAX_FRAMES  (PIPE_RATE * 10)  // sanity cap: reject clips > 10 s
#define CLIP_RATE_MIN    4000
#define CLIP_RATE_MAX    48000

static StreamBufferHandle_t s_clip_fifo;
static QueueHandle_t        s_clip_req;     // char[CLIP_NAME_MAX], len 1, overwrite

// GSFX header, see tools/make_clip.py.
typedef struct __attribute__((packed)) {
    char     magic[4];      // "GSFX"
    uint32_t sample_rate;
    uint32_t num_frames;
} gsfx_header_t;

// True if a newer clip request is already waiting; abort the current stream.
static bool clip_preempted(void)
{
    return uxQueueMessagesWaiting(s_clip_req) > 0;
}

// Stream one clip file: header-check, then read, linear-resample to 44.1 kHz,
// and push to the FIFO. Aborts early if a newer request arrives.
static void stream_clip(const char *name)
{
    char path[16 + CLIP_NAME_MAX];
    snprintf(path, sizeof(path), "%s/%s.gsfx", FS_CLIPS_MOUNT, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "clip not found: %s", path);
        return;
    }

    gsfx_header_t hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        memcmp(hdr.magic, "GSFX", 4) != 0 ||
        hdr.sample_rate < CLIP_RATE_MIN || hdr.sample_rate > CLIP_RATE_MAX ||
        hdr.num_frames == 0 || hdr.num_frames > CLIP_MAX_FRAMES) {
        ESP_LOGW(TAG, "bad GSFX header: %s", path);
        fclose(f);
        return;
    }
    ESP_LOGI(TAG, "play %s: %u Hz, %u frames", path,
             (unsigned)hdr.sample_rate, (unsigned)hdr.num_frames);

    // Streaming linear resampler. step = src/out samples per output frame.
    const float step = (float)hdr.sample_rate / (float)PIPE_RATE;
    int16_t src[SRC_CHUNK];
    size_t  src_have = 0, src_idx = 0;   // current read-buffer window
    bool    eof = false;

    // Pull the next source sample, refilling from the file as needed.
    // Returns false at end of stream.
    #define SRC_NEXT(outvar) ({                                            \
        bool _ok = true;                                                   \
        if (src_idx >= src_have) {                                         \
            if (eof) { _ok = false; }                                      \
            else {                                                         \
                size_t _n = fread(src, sizeof(int16_t), SRC_CHUNK, f);     \
                if (_n == 0) { eof = true; _ok = false; }                  \
                else { src_have = _n; src_idx = 0;                         \
                       if (_n < SRC_CHUNK) eof = true; }                   \
            }                                                              \
        }                                                                  \
        if (_ok) { (outvar) = src[src_idx++]; }                            \
        _ok;                                                               \
    })

    int16_t prev = 0, next = 0;
    if (!SRC_NEXT(prev)) { fclose(f); return; }
    if (!SRC_NEXT(next)) next = prev;   // single-sample clip
    float t = 0.0f;

    int16_t out[SRC_CHUNK];
    size_t  out_n = 0;

    for (;;) {
        // Produce one interpolated output frame.
        float s = (float)prev + ((float)next - (float)prev) * t;
        out[out_n++] = (int16_t)lrintf(s);
        t += step;
        while (t >= 1.0f) {
            prev = next;
            if (!SRC_NEXT(next)) { next = prev; t = 0.0f; goto flush_tail; }
            t -= 1.0f;
        }
        if (out_n == SRC_CHUNK) {
            // Blocking send paces the feeder to the consumer; bail on preempt.
            xStreamBufferSend(s_clip_fifo, out, out_n * sizeof(int16_t),
                              pdMS_TO_TICKS(200));
            out_n = 0;
            if (clip_preempted()) { fclose(f); return; }
        }
    }
flush_tail:
    if (out_n) {
        xStreamBufferSend(s_clip_fifo, out, out_n * sizeof(int16_t),
                          pdMS_TO_TICKS(200));
    }
    #undef SRC_NEXT
    fclose(f);
}

static void feeder_task(void *arg)
{
    char name[CLIP_NAME_MAX];
    for (;;) {
        if (xQueueReceive(s_clip_req, name, portMAX_DELAY) != pdTRUE) continue;
        // Drop any stale tail of a previous clip so the new one starts clean.
        xStreamBufferReset(s_clip_fifo);
        stream_clip(name);
    }
}

// ---- synth cue (audio-task side) ------------------------------------------
// A synth cue is a list of notes played in sequence with a short envelope,
// generated from a sine LUT in sfx_generate_block; no file I/O, no FIFO.

typedef struct { float freq; uint32_t frames; } synth_note_t;

#define NF(ms) ((uint32_t)(PIPE_RATE * (ms) / 1000))   // note length in frames

// Distinct cues so the user can tell transitions apart by ear.
static const synth_note_t CUE_PAIRING[]    = {            // E5 to A5 rising
    { 659.25f, NF(110) }, { 880.00f, NF(170) } };
static const synth_note_t CUE_CONNECT[]    = {            // C5 E5 G5 rising triad
    { 523.25f, NF(90) }, { 659.25f, NF(90) }, { 783.99f, NF(190) } };
static const synth_note_t CUE_DISCONNECT[] = {            // G5 to C5 falling
    { 783.99f, NF(110) }, { 523.25f, NF(220) } };
static const synth_note_t CUE_REBOND[]     = {            // A4 to E5 rising blip
    { 440.00f, NF(70) }, { 659.25f, NF(110) } };
static const synth_note_t CUE_MODE[]       = {            // C5 G5 C6 mode arp
    { 523.25f, NF(80) }, { 783.99f, NF(80) }, { 1046.50f, NF(160) } };

static const struct { const synth_note_t *notes; int count; } SYNTH_CUES[] = {
    [SFX_SYNTH_PAIRING]    = { CUE_PAIRING,    2 },
    [SFX_SYNTH_CONNECT]    = { CUE_CONNECT,    3 },
    [SFX_SYNTH_DISCONNECT] = { CUE_DISCONNECT, 2 },
    [SFX_SYNTH_REBOND]     = { CUE_REBOND,     2 },
    [SFX_SYNTH_MODE]       = { CUE_MODE,       3 },
};

#define SINE_LUT_SIZE  1024                 // power of 2 for index mask, no modulo
#define SINE_LUT_MASK  (SINE_LUT_SIZE - 1)
#define SYNTH_AMP      (0.6f * 32767.0f)
#define ATK_SAMPLES    (PIPE_RATE / 200)     // 5 ms linear attack

static float s_sine[SINE_LUT_SIZE];          // built once in sfx_init

static SemaphoreHandle_t s_lock;
static volatile bool      s_synth_pending;   // a trigger is waiting to start
static volatile synth_id_t s_pending_id;     // which cue the pending trigger wants

// render-private synth state (only touched by the audio task). The oscillator
// reads a precomputed sine LUT and the envelope is a running multiply: no
// per-sample sinf/expf (which would spike DSP load and underrun the BT stream).
// One expf per note (the decay coefficient) only.
static bool      s_synth_active;
static const synth_note_t *s_notes;
static int       s_note_count;
static int       s_note_idx;
static uint32_t  s_note_pos;     // sample index within the current note
static float     s_phase;        // LUT index, 0..SINE_LUT_SIZE
static float     s_phase_inc;    // LUT indices per sample for this note
static float     s_env;          // running decay envelope (x s_decay per sample)
static float     s_decay;        // per-sample decay multiplier for this note

// This block's mixed cue (synth + clip), int16-scale float. Generated once per
// block by sfx_generate_block(); the DSP mixes it into both the BT and speaker
// outputs, so a single cue advance feeds both.
static float     s_cue[MAX_BLOCK];
static bool      s_cue_active;

void sfx_trigger_synth(synth_id_t id)
{
    if (id < 0 || id >= SFX_SYNTH_COUNT) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_pending_id = id;
    s_synth_pending = true;
    xSemaphoreGive(s_lock);
}

void sfx_trigger_clip(const char *name)
{
    if (!name || !s_clip_req) return;
    char buf[CLIP_NAME_MAX];
    strncpy(buf, name, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    // Overwrite: newest request wins and preempts any in-flight clip.
    xQueueOverwrite(s_clip_req, buf);
}

// Set up oscillator + envelope for note s_note_idx; deactivates at the end.
static void synth_start_note(void)
{
    if (s_note_idx >= s_note_count) { s_synth_active = false; return; }
    const synth_note_t *n = &s_notes[s_note_idx];
    s_phase     = 0.0f;
    s_phase_inc = (float)n->freq * (float)SINE_LUT_SIZE / (float)PIPE_RATE;
    s_decay     = expf(-3.0f / (float)n->frames);   // one expf per note
    s_env       = 1.0f;
    s_note_pos  = 0;
}

// One synth sample (int16-scale float) from the LUT, advancing phase/envelope.
static inline float synth_next(void)
{
    if (!s_synth_active) return 0.0f;
    const synth_note_t *n = &s_notes[s_note_idx];
    float atk = (s_note_pos < (uint32_t)ATK_SAMPLES)
                    ? ((float)s_note_pos / (float)ATK_SAMPLES) : 1.0f;
    float s = SYNTH_AMP * atk * s_env * s_sine[(int)s_phase & SINE_LUT_MASK];

    s_env  *= s_decay;
    s_phase += s_phase_inc;
    if (s_phase >= (float)SINE_LUT_SIZE) s_phase -= (float)SINE_LUT_SIZE;
    if (++s_note_pos >= n->frames) {
        s_note_idx++;
        synth_start_note();
    }
    return s;
}

// Render this block's cue once into s_cue[] (advancing cue state a single
// time), so the DSP can mix the SAME samples into both outputs.
void sfx_generate_block(size_t frames)
{
    if (frames == 0) return;
    if (frames > MAX_BLOCK) frames = MAX_BLOCK;

    // Latch a pending synth trigger at a block boundary.
    if (s_synth_pending) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_synth_pending = false;
        synth_id_t id = s_pending_id;
        xSemaphoreGive(s_lock);
        s_synth_active = true;
        s_notes = SYNTH_CUES[id].notes;
        s_note_count = SYNTH_CUES[id].count;
        s_note_idx = 0;
        synth_start_note();
    }

    bool any = false;
    for (size_t i = 0; i < frames; i++) s_cue[i] = 0.0f;

    if (s_synth_active) {
        for (size_t i = 0; i < frames && s_synth_active; i++) {
            s_cue[i] += synth_next();
        }
        any = true;
    }

    // Clip cue: drain whatever the feeder has resampled into the FIFO.
    if (s_clip_fifo && xStreamBufferBytesAvailable(s_clip_fifo) > 0) {
        int16_t tmp[MAX_BLOCK];
        size_t got = xStreamBufferReceive(s_clip_fifo, tmp,
                                          frames * sizeof(int16_t), 0)
                     / sizeof(int16_t);
        for (size_t i = 0; i < got; i++) s_cue[i] += (float)tmp[i];
        if (got) any = true;
    }

    s_cue_active = any;
}

const float *sfx_cue(void)      { return s_cue; }
bool         sfx_cue_active(void) { return s_cue_active; }

esp_err_t sfx_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_clip_fifo = xStreamBufferCreate(CLIP_FIFO_BYTES, sizeof(int16_t));
    s_clip_req  = xQueueCreate(1, CLIP_NAME_MAX);
    if (!s_lock || !s_clip_fifo || !s_clip_req) return ESP_ERR_NO_MEM;

    // Build the sine LUT once (the only sinf calls; none per audio sample).
    for (int i = 0; i < SINE_LUT_SIZE; i++) {
        s_sine[i] = sinf(2.0f * (float)M_PI * (float)i / (float)SINE_LUT_SIZE);
    }

    // Priority 8: below the audio pipeline (10); it only needs to keep the
    // FIFO ahead of the per-block drain, and does blocking file I/O.
    // Pinned to core 1 with the rest of the audio chain, off the BT core (0).
    BaseType_t ok = xTaskCreatePinnedToCore(feeder_task, "sfx_feed", 4096, NULL, 8, NULL, 1);
    if (ok != pdPASS) return ESP_FAIL;

    ESP_LOGI(TAG, "cue player ready");
    return ESP_OK;
}

