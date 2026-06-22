#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Full-duplex I2S0 master bus to the ES8388 codec.
//
// The ESP32 is the I2S master: it generates MCLK (from the APLL on GPIO0, 256
// fs), BCLK, and LRCK, all shared between the capture (RX) and playback (TX)
// directions. The ES8388 is the I2S slave: its ADC drives ASDOUT into our DIN,
// and its DAC reads DSDIN from our DOUT, both clocked off the BCLK/LRCK we
// provide. One clock domain for both directions, so there is no async resampling
// FIFO.
//
// Slots are configured 32-in-32 (Philips). The ES8388 is set to a 24/32-bit I2S
// word length; the captured 24-bit sample sits in bits 31:8 of each int32 slot,
// matching the down-convert (`>> 16`) in audio_pipeline.c.
//
// The codec must be told to be an I2S slave at the matching word length over I2C
// (see es8388.h). Bring the I2S clocks up first so the ES8388 has MCLK before its
// I2C configuration runs.

esp_err_t i2s_codec_init(void);
esp_err_t i2s_codec_start(void);
esp_err_t i2s_codec_stop(void);

// Delete both I2S channels (releasing the APLL). Channels must be stopped
// (i2s_codec_stop) first. Pair with i2s_codec_init() to fully rebuild the bus,
// needed after a CPU light-sleep session, which powers down the APLL: a bare
// i2s_codec_start() then re-enables the channels against a dead or mis-locked
// APLL, giving wrong BCLK/MCLK (garbled audio + an un-paced read loop). A full
// delete+recreate re-locks the APLL from scratch.
esp_err_t i2s_codec_deinit(void);

// Blocking read from the ADC (RX DMA). Frame size is 8 bytes (stereo, 32-bit
// slots). Returns ESP_ERR_TIMEOUT if no full frame arrives before timeout_ms
// expires.
esp_err_t i2s_codec_read(void *buf, size_t size_bytes,
                         size_t *bytes_read, uint32_t timeout_ms);

// Blocking write to the DAC (TX DMA). Same 8-byte stereo 32-bit frame layout.
// Paces the caller to the shared 44.1 kHz clock.
esp_err_t i2s_codec_write(const void *buf, size_t size_bytes,
                          size_t *bytes_written, uint32_t timeout_ms);
