#pragma once

// Single source of truth for all pin assignments. Do not hard-code GPIO
// numbers anywhere else.
//
// Board: ESP32-PICO-MINI-02 with the ES8388 codec on a full-duplex I2S master
// bus plus I2C control.

// ----- I2S0: full-duplex master to the ES8388 -----
// The ESP32 masters the bus and generates MCLK, BCLK, and LRCK; the ES8388 is
// the I2S slave. MCLK comes off the APLL on GPIO0 (the only free MCLK-capable
// pin; GPIO1/3 are the console UART) and reaches the codec through a series
// resistor. DIN/DOUT are named from the ESP32 side: our DOUT drives the codec
// DAC input (DSDIN), our DIN reads the codec ADC output (ASDOUT).
#define PIN_I2S_MCLK     0   // ES8388 MCLK (APLL)
#define PIN_I2S_BCK     25   // shared bit clock
#define PIN_I2S_LRCK    26   // shared word/LR clock
#define PIN_I2S_DOUT    27   // to ES8388 DSDIN (DAC playback)
#define PIN_I2S_DIN     14   // from ES8388 ASDOUT (ADC capture)

// ----- I2C0: ES8388 control (address 0x10) -----
#define PIN_I2C_SDA     21
#define PIN_I2C_SCL     22

// ----- Analog input: GBA volume wheel (VR2 wiper) -----
// Read on ADC1; GPIO39 is input-only. In Mode B the wheel sets the DSP digital
// volume; in Mode A it sets the ES8388 analog-bypass volume over I2C. The wheel
// is powered from VBAT, so its reading is battery-referenced (see PIN_VBAT_ADC).
#define PIN_VOL_ADC     39

// ----- Analog input: battery-rail sense -----
// VBAT (the GBA battery rail feeding the U3 boost) through an R20/R21 = 100k/100k
// divider into ADC1_CH0 (GPIO36, input-only). VBAT = 2 * V(PIN_VBAT_ADC). Shares
// the ADC1 oneshot unit with the volume wheel.
#define PIN_VBAT_ADC    36

// ----- Control outputs -----
// External mono speaker-amp shutdown/mute (active HIGH = amp enabled).
// app_sm asserts mute when BT is STREAMING or wired HP is plugged.
#define PIN_PAM_SD       4

// ----- Wake-source inputs (RTC GPIOs, input-only, external 10k pull-ups) -----
// Connect/Pair button: the mod's control input (connect / pair, deep-sleep
// wake, boot-time factory reset). Tapped off the GBA R shoulder net.
#define PIN_CP_BUTTON   34   // LOW = pressed (idle pulled HIGH by 10k)

// HP-detect. Polarity is inverted vs a typical detect line: the GBA's internal
// headphone-jack switch grounds this sense line when nothing is plugged in.
// With the external 10k pull-up to 3.3 V, the ESP32 reads HIGH = plugged,
// LOW = unplugged.
#define PIN_HP_DETECT   35   // HIGH = headphones plugged, LOW = unplugged
