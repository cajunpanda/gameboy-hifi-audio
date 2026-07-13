#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// ES8388 codec control driver (I2C).
//
// The ES8388 is the audio front-and-back end: stereo line-in capture (ADC),
// stereo DAC playback, a built-in headphone amplifier (LOUT1/ROUT1), a line
// output for the external mono speaker amp (LOUT2/ROUT2), and an internal analog
// bypass (LIN -> mixer -> outputs) for the low-power "battery" mode. It is the
// I2S slave; the ESP32 masters the bus and supplies MCLK (see i2s_codec.h).
//
// I2C control only; the audio samples flow over I2S. Call es8388_init() after
// i2s_codec_start() so the chip already sees MCLK when it is configured.
//
// The codec must be configured with MCLK live (its state machine won't start
// otherwise). MCLK (~22.6 MHz at the pin) can couple into SDA/SCL, but on the
// production PCB the ~150 ohm series R on MCLK + short MCLK routing keep the I2C
// writes clean, so es8388_init() alone configures reliably (no read-back verify).

// Mode A vs Mode B. Orthogonal to which output the headphone jack selects.
typedef enum {
    ES8388_MODE_BYPASS,   // Mode A: analog LIN -> outputs, ADC/DAC/digital off
    ES8388_MODE_DSP,      // Mode B: LIN -> ADC -> I2S -> ESP32 DSP -> I2S -> DAC -> outputs
} es8388_mode_t;

// Which analog output stage is driven. The mono speaker amp is muted in firmware
// via its own SD pin (PIN_PAM_SD) on top of this routing.
typedef enum {
    ES8388_OUT_SPEAKER,   // line out (LOUT2/ROUT2) -> external mono amp
    ES8388_OUT_HEADPHONE, // HP amp (LOUT1/ROUT1) -> jack
    ES8388_OUT_BOTH,
} es8388_out_t;

// I2S-DMA service callback: drains the ADC and feeds the DAC one I2S block.
// es8388_init() invokes it between I2C ops so the I2S DMA stays serviced while
// the codec is configured with MCLK live; an unserviced DMA storms the CPU and
// trips the interrupt watchdog. Pass NULL to skip (only safe if I2S isn't
// running, or something else is draining it).
typedef void (*es8388_service_cb_t)(void);

// One-shot: bring up the I2C bus, probe the chip, and load the default register
// set (Mode B, ADC from line-in, DAC to both outputs, I2S slave 24/32-bit). Call
// after i2s_codec_start() (the codec needs MCLK). Returns ESP_OK with the codec
// configured and unmuted.
esp_err_t es8388_init(es8388_service_cb_t service);

// Select the analog signal path (Mode A bypass vs Mode B ADC<->DAC).
esp_err_t es8388_set_mode(es8388_mode_t mode);

// Route the active signal to the headphone amp, the line out (speaker), or both.
// Called on HP-detect edges.
esp_err_t es8388_route_output(es8388_out_t out);

// Output volume, 0..100 %. In Mode B this trims the DAC; in Mode A it trims the
// analog-bypass mixer/output stage (the codec replaces the wheel pot). The VOL
// wheel maps here in Mode A.
esp_err_t es8388_set_output_volume(int percent);

// Raw register access (debug).
esp_err_t es8388_write_reg(uint8_t reg, uint8_t val);
esp_err_t es8388_read_reg(uint8_t reg, uint8_t *val);
