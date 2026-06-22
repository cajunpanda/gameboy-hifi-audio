#include "i2s_codec.h"

#include <inttypes.h>

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"

#include "pinmap.h"

static const char *TAG = "i2s_codec";

static i2s_chan_handle_t s_tx_chan = NULL;
static i2s_chan_handle_t s_rx_chan = NULL;

// DMA sizing: small buffers keep end-to-end A2DP latency low. 4 x 240 ~ 21 ms of
// slack each way at 44.1 kHz, comfortably serviced by the audio_pipeline task at
// priority 10.
#define DMA_DESC_NUM   4
#define DMA_FRAME_NUM  240

esp_err_t i2s_codec_init(void)
{
    // One channel config, both directions: a shared full-duplex bus on
    // I2S_NUM_0. The TX and RX handles share MCLK/BCLK/LRCK; only DOUT/DIN differ
    // (set per-direction in the std config below).
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = DMA_DESC_NUM;
    chan_cfg.dma_frame_num = DMA_FRAME_NUM;
    ESP_RETURN_ON_ERROR(
        i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan),
        TAG, "i2s_new_channel");

    // 32-in-32 Philips slots. The ES8388 is configured (over I2C) for a 24/32-bit
    // I2S word length; the 24 valid bits land in 31:8 of each int32 slot, and
    // audio_pipeline.c down-converts with `>> 16`.
    i2s_std_slot_config_t slot = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);

    // Master clock from the APLL, 256 fs MCLK on GPIO0.
    i2s_std_clk_config_t clk = {
        .sample_rate_hz = 44100,
        .clk_src        = I2S_CLK_SRC_APLL,
        .mclk_multiple  = I2S_MCLK_MULTIPLE_256,
    };

    i2s_std_config_t tx_cfg = {
        .clk_cfg  = clk,
        .slot_cfg = slot,
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCK,
            .ws   = PIN_I2S_LRCK,
            .dout = PIN_I2S_DOUT,        // -> ES8388 DSDIN (DAC)
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { 0 },
        },
    };
    ESP_RETURN_ON_ERROR(
        i2s_channel_init_std_mode(s_tx_chan, &tx_cfg),
        TAG, "i2s tx init");

    i2s_std_config_t rx_cfg = {
        .clk_cfg  = clk,
        .slot_cfg = slot,
        .gpio_cfg = {
            // MCLK/BCLK/WS are driven by the TX channel on the shared bus;
            // the RX channel reuses the same pins (the driver detects the
            // shared assignment) and only adds DIN.
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCK,
            .ws   = PIN_I2S_LRCK,
            .dout = I2S_GPIO_UNUSED,
            .din  = PIN_I2S_DIN,         // <- ES8388 ASDOUT (ADC)
            .invert_flags = { 0 },
        },
    };
    ESP_RETURN_ON_ERROR(
        i2s_channel_init_std_mode(s_rx_chan, &rx_cfg),
        TAG, "i2s rx init");

    ESP_LOGI(TAG, "I2S0 full-duplex master configured: 44.1 kHz, 32-in-32, MCLK GPIO%d (APLL)",
             PIN_I2S_MCLK);
    return ESP_OK;
}

esp_err_t i2s_codec_start(void)
{
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_chan), TAG, "tx enable");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_chan), TAG, "rx enable");
    ESP_LOGI(TAG, "I2S0 RX+TX enabled");

    // Report the clocks the driver believes it programmed. On ESP32 + APLL the
    // IDF I2S driver's MCLK divider is unreliable: get_info reports 256 fs
    // (11.29 MHz) but a scope on GPIO0 reads ~22.6 MHz (512 fs), the APLL with no
    // /2. The ES8388 tolerates the ratio (it clocks data off BCLK/LRCK), so audio
    // is clean regardless, but do not trust this MCLK number; scope it. The
    // 22.6 MHz is also the EMI aggressor that couples into the I2C bus.
    i2s_chan_info_t info;
    if (i2s_channel_get_info(s_tx_chan, &info) == ESP_OK) {
        ESP_LOGI(TAG, "clocks (driver-reported; MCLK unreliable, scope it): APLL=%" PRIu32
                 " Hz  MCLK=%" PRIu32 " Hz  BCLK=%" PRIu32 " Hz",
                 info.sclk_hz, info.mclk_hz, info.bclk_hz);
    }
    return ESP_OK;
}

esp_err_t i2s_codec_stop(void)
{
    esp_err_t e1 = i2s_channel_disable(s_rx_chan);
    esp_err_t e2 = i2s_channel_disable(s_tx_chan);
    return (e1 != ESP_OK) ? e1 : e2;
}

esp_err_t i2s_codec_deinit(void)
{
    // Delete RX before TX (RX is the secondary on the shared bus). Each must be
    // disabled already (i2s_codec_stop). Null the handles so a following
    // i2s_codec_init() recreates them cleanly and the APLL is re-acquired fresh.
    esp_err_t e1 = ESP_OK, e2 = ESP_OK;
    if (s_rx_chan) { e1 = i2s_del_channel(s_rx_chan); s_rx_chan = NULL; }
    if (s_tx_chan) { e2 = i2s_del_channel(s_tx_chan); s_tx_chan = NULL; }
    return (e1 != ESP_OK) ? e1 : e2;
}

esp_err_t i2s_codec_read(void *buf, size_t size_bytes,
                         size_t *bytes_read, uint32_t timeout_ms)
{
    return i2s_channel_read(s_rx_chan, buf, size_bytes, bytes_read,
                            timeout_ms);
}

esp_err_t i2s_codec_write(const void *buf, size_t size_bytes,
                          size_t *bytes_written, uint32_t timeout_ms)
{
    return i2s_channel_write(s_tx_chan, buf, size_bytes, bytes_written,
                             timeout_ms);
}
