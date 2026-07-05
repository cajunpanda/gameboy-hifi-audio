#include "es8388.h"

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"   // esp_rom_delay_us (inter-retry settle)

#include "pinmap.h"

static const char *TAG = "es8388";

// 7-bit I2C address. The ES8388's CE pin selects 0x10 (CE low) or 0x11 (CE
// high); the module wires it to 0x10.
#define ES8388_I2C_ADDR   0x10
#define ES8388_I2C_HZ     100000
#define I2C_TIMEOUT_MS    100
// A few write-retries as cheap insurance on a marginal bus. The real cure is a
// decoupling cap at the codec (0.1 uF DVDD-to-GND); without it the bus can
// corrupt register data while still ACKing, which retries can't catch.
#define ES8388_WR_RETRIES 4
#define ES8388_WR_RETRY_US 200

// ---- register map (subset we touch) ---------------------------------------
#define ES8388_CONTROL1     0x00
#define ES8388_CONTROL2     0x01
#define ES8388_CHIPPOWER    0x02
#define ES8388_ADCPOWER     0x03
#define ES8388_DACPOWER     0x04
#define ES8388_CHIPLOPOW1   0x05
#define ES8388_CHIPLOPOW2   0x06
#define ES8388_ANAVOLMANAG  0x07
#define ES8388_MASTERMODE   0x08
#define ES8388_ADCCONTROL1  0x09
#define ES8388_ADCCONTROL2  0x0a
#define ES8388_ADCCONTROL3  0x0b
#define ES8388_ADCCONTROL4  0x0c
#define ES8388_ADCCONTROL5  0x0d
#define ES8388_ADCCONTROL8  0x10   // L ADC digital volume
#define ES8388_ADCCONTROL9  0x11   // R ADC digital volume
#define ES8388_DACCONTROL1  0x17
#define ES8388_DACCONTROL2  0x18
#define ES8388_DACCONTROL3  0x19   // DAC mute
#define ES8388_DACCONTROL4  0x1a   // L DAC digital volume
#define ES8388_DACCONTROL5  0x1b   // R DAC digital volume
#define ES8388_DACCONTROL16 0x26   // L/R mixer select
#define ES8388_DACCONTROL17 0x27   // LD2LO + left mixer gain (bypass routing)
#define ES8388_DACCONTROL18 0x28   // left mixer (datasheet codec setup)
#define ES8388_DACCONTROL19 0x29   // right mixer (datasheet codec setup)
#define ES8388_DACCONTROL20 0x2a   // RD2RO + right mixer gain (bypass routing)
#define ES8388_DACCONTROL21 0x2b   // MCLK-input enable + DAC/ADC shared LRCK
#define ES8388_DACCONTROL24 0x2e   // LOUT1 (HP L) volume
#define ES8388_DACCONTROL25 0x2f   // ROUT1 (HP R) volume
#define ES8388_DACCONTROL26 0x30   // LOUT2 (line L -> speaker amp) volume
#define ES8388_DACCONTROL27 0x31   // ROUT2 (line R -> speaker amp) volume

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;

// Track the active path so es8388_route_output() can compute the DACPOWER (0x04)
// register correctly: in BYPASS the DAC is powered down (bits 7:6 set) while the
// output drivers stay enabled; in DSP the DAC is on (bits 7:6 clear).
// es8388_init() leaves the codec in DSP; es8388_set_mode() updates this.
static es8388_mode_t s_mode = ES8388_MODE_DSP;

esp_err_t es8388_write_reg(uint8_t reg, uint8_t val)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    uint8_t buf[2] = { reg, val };
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < ES8388_WR_RETRIES; i++) {
        err = i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
        if (err == ESP_OK) return ESP_OK;
        esp_rom_delay_us(ES8388_WR_RETRY_US);
    }
    return err;   // still failing after retries: genuinely bad bus
}

esp_err_t es8388_read_reg(uint8_t reg, uint8_t *val)
{
    if (!s_dev || !val) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, I2C_TIMEOUT_MS);
}

// Tuned output-driver levels (6-bit field; dB = reg*1.5-45), set by ear on the
// bench. Single source of truth: es8388_init() uses these as the fixed Mode B
// driver levels, and es8388_set_output_volume() uses them as the
// Mode A analog-volume ceilings at full wheel. Keeping them shared makes the
// loudness match across a B<->A switch: the PAM8302A's ~+23.5 dB fixed gain would
// overdrive the 8 ohm speaker at line level, so the speaker sits at -10.5 dB, and
// the HP amp drives headphones directly so it sits a bit lower at -15 dB.
#define ES8388_LINE_VOL_MAX 0x17   // LOUT2/ROUT2 speaker line out ~ -10.5 dB
#define ES8388_HP_VOL_MAX   0x14   // LOUT1/ROUT1 headphone amp    ~ -15 dB

// Map 0..100 % to a 6-bit output-volume register, scaled to the given ceiling.
static uint8_t vol_to_reg_max(int percent, uint8_t reg_max)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    return (uint8_t)((percent * reg_max) / 100);
}

esp_err_t es8388_init(es8388_service_cb_t service)
{
    if (!s_bus) {
        i2c_master_bus_config_t bus_cfg = {
            .i2c_port   = I2C_NUM_0,
            .sda_io_num = PIN_I2C_SDA,
            .scl_io_num = PIN_I2C_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus),
                            TAG, "i2c bus");

        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = ES8388_I2C_ADDR,
            .scl_speed_hz    = ES8388_I2C_HZ,
        };
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev),
                            TAG, "i2c dev");
    }

    // Retry the probe: on a marginal bus a single probe can NACK even though the
    // chip is present, and a hard fail here aborts boot (ESP_ERROR_CHECK) into a
    // boot loop. The chip only has to answer once.
    bool found = false;
    for (int i = 0; i < 20; i++) {
        if (i2c_master_probe(s_bus, ES8388_I2C_ADDR, I2C_TIMEOUT_MS) == ESP_OK) {
            found = true;
            break;
        }
        esp_rom_delay_us(2000);
    }
    if (!found) {
        ESP_LOGE(TAG, "ES8388 not found at 0x%02x after retries", ES8388_I2C_ADDR);
        return ESP_ERR_NOT_FOUND;
    }

    // ES8388 "start up codec" sequence, in the order the datasheet mandates
    // ("must be done step by step"): power the DEM/STM digital engine down first
    // (0x02=0xF3), configure, then power it up as the very last step (0x02=0x00).
    // Powering up early leaves the DAC state machine stopped and the chip silent.
    // Configured for this front end: LIN2/RIN2 line input at 0 dB PGA (no mic
    // boost; ALC off), DAC -> LOUT1/ROUT1 (HP jack) + LOUT2/ROUT2 (line out).
    // 256 fs MCLK assumed (i2s_codec.c).
    const struct { uint8_t reg, val; } seq[] = {
        { ES8388_MASTERMODE,   0x00 }, // I2S slave (ESP32 masters the bus)
        { ES8388_CHIPPOWER,    0xf3 }, // power down DEM + STM during config
        { ES8388_DACCONTROL21, 0x80 }, // DAC + ADC share one LRCK
        { ES8388_CONTROL1,     0x05 }, // play & record mode, start reference
        { ES8388_CONTROL2,     0x40 }, // power up analog + ibias
        { ES8388_ADCPOWER,     0x00 }, // power up ADC + analog input
        // -------- ADC (capture: LIN2/RIN2 line in) --------
        { ES8388_ADCCONTROL2,  0x50 }, // input select = LIN2/RIN2 (line)
        { ES8388_ADCCONTROL1,  0x00 }, // ADC PGA = 0 dB (line level, no boost)
        { ES8388_ADCCONTROL4,  0x00 }, // ADC SFI: I2S, 24-bit
        { ES8388_ADCCONTROL5,  0x02 }, // ADC MCLK/LRCK = 256
        { ES8388_ADCCONTROL8,  0x00 }, // LADC vol 0 dB
        { ES8388_ADCCONTROL9,  0x00 }, // RADC vol 0 dB
        // -------- DAC (playback) --------
        { ES8388_DACPOWER,     0x3c }, // power up DAC + enable LOUT1/2 ROUT1/2
        { ES8388_DACCONTROL1,  0x00 }, // DAC SFI: I2S, 24-bit
        { ES8388_DACCONTROL2,  0x02 }, // DAC MCLK/LRCK = 256
        { ES8388_DACCONTROL4,  0x00 }, // LDAC vol 0 dB
        { ES8388_DACCONTROL5,  0x00 }, // RDAC vol 0 dB
        { ES8388_DACCONTROL3,  0x32 }, // DAC unmute (soft-ramp, datasheet default)
        // -------- output mixer: DAC -> L/R output (datasheet mixer setup) --
        { ES8388_DACCONTROL16, 0x00 },
        { ES8388_DACCONTROL17, 0xb8 }, // LDAC -> left output
        { ES8388_DACCONTROL18, 0x38 },
        { ES8388_DACCONTROL19, 0x38 },
        { ES8388_DACCONTROL20, 0xb8 }, // RDAC -> right output
        // -------- output driver volumes --------
        // From the shared ES8388_*_VOL_MAX constants (tuned by ear), so the fixed
        // Mode B levels here and the Mode A wheel ceilings stay in lockstep.
        { ES8388_DACCONTROL24, ES8388_HP_VOL_MAX },   // LOUT1 (HP L)  ~ -15 dB
        { ES8388_DACCONTROL25, ES8388_HP_VOL_MAX },   // ROUT1 (HP R)  ~ -15 dB
        { ES8388_DACCONTROL26, ES8388_LINE_VOL_MAX }, // LOUT2 (line L -> speaker amp) ~ -10.5 dB
        { ES8388_DACCONTROL27, ES8388_LINE_VOL_MAX }, // ROUT2 (line R -> speaker amp) ~ -10.5 dB
        // -------- final: power up the DEM + STM digital engine (must be last) -
        { ES8388_CHIPPOWER,    0x00 }, // run the DAC/ADC state machine
    };
    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        // Service the I2S DMA between writes: MCLK is live and nothing else drains
        // the DMA yet, so an unserviced burst storms CPU0 and trips the interrupt
        // watchdog. This also paces the writes and lightens MCLK-coupling
        // corruption (the PCB's series-R + short routing keeps what's left benign).
        if (service) service();
        es8388_write_reg(seq[i].reg, seq[i].val);
    }

    s_mode = ES8388_MODE_DSP;   // init leaves the digital ADC<->DAC path active
    ESP_LOGI(TAG, "ES8388 configured (codec mode: LIN2/RIN2 line in, DAC -> HP + line out)");
    return ESP_OK;
}

esp_err_t es8388_set_mode(es8388_mode_t mode)
{
    // Transition the already-initialized codec (es8388_init leaves it in Mode B /
    // DSP) between the digital ADC<->DAC path and the analog bypass path. The line
    // input is LIN2/RIN2, so LMIXSEL/RMIXSEL select LIN2/RIN2 (reg 0x26 = 0x09).
    //
    // No read-back verification here, deliberately. On a marginal bus (MCLK
    // coupling) heavy back-to-back I2C reads hang the bus, and the IDF driver's
    // bus-recovery then trips the interrupt watchdog. A mode switch is
    // user-initiated and instantly re-runnable, so es8388_write_reg's NACK-retry
    // is enough; a corrupted write just means an audibly-wrong switch you redo.
    //
    // Transitions may click (output routing flips live); add output-mute ramps if
    // objectionable.
    switch (mode) {
    case ES8388_MODE_BYPASS:
        // Establish the analog path first, then drop the digital blocks so audio
        // never glitches: route bypass to outputs, then power down ADC/DAC/STM.
        es8388_write_reg(ES8388_DACCONTROL16, 0x09); // LMIXSEL/RMIXSEL = LIN2/RIN2
        es8388_write_reg(ES8388_DACCONTROL17, 0x50); // left:  LI2LO on, LD2LO off (bypass->LOUT)
        es8388_write_reg(ES8388_DACCONTROL20, 0x50); // right: RI2RO on, RD2RO off (bypass->ROUT)
        es8388_write_reg(ES8388_ADCPOWER,     0x3f); // keep analog input PGA, power down ADC
        es8388_write_reg(ES8388_DACPOWER,     0xfc); // power down DAC, keep all 4 output drivers
        es8388_write_reg(ES8388_CHIPPOWER,    0xf3); // power down DEM + STM (digital engine off)
        s_mode = ES8388_MODE_BYPASS;
        ESP_LOGI(TAG, "mode: analog BYPASS (Mode A) - LIN2/RIN2 -> outputs, digital off");
        break;
    case ES8388_MODE_DSP:
        // Restore the es8388_init values. Re-route DAC to outputs and power the
        // converters back up, then start the digital engine (STM) last, the same
        // order es8388_init uses (the DAC state machine won't run cleanly if
        // started before the path is configured). Caller must have resumed
        // I2S/MCLK.
        es8388_write_reg(ES8388_DACCONTROL16, 0x00); // mixer source back to default
        es8388_write_reg(ES8388_DACCONTROL17, 0xb8); // left:  LDAC->output, bypass off
        es8388_write_reg(ES8388_DACCONTROL20, 0xb8); // right: RDAC->output, bypass off
        es8388_write_reg(ES8388_ADCPOWER,     0x00); // power up ADC + analog input
        es8388_write_reg(ES8388_DACPOWER,     0x3c); // power up DAC + enable outputs
        es8388_write_reg(ES8388_CHIPPOWER,    0x00); // run DEM + STM (last)
        s_mode = ES8388_MODE_DSP;
        ESP_LOGI(TAG, "mode: DSP (Mode B) - ADC<->DAC digital path");
        break;
    }
    return ESP_OK;
}

esp_err_t es8388_route_output(es8388_out_t out)
{
    // Gate the output drivers via DACPOWER (0x04): bit5 LOUT1 + bit4 ROUT1 are the
    // HP amp; bit3 LOUT2 + bit2 ROUT2 are the line out to the external speaker
    // amp. Powering down the unused pair saves a little current and keeps a
    // disconnected stage quiet. The DAC power-down bits (7:6) depend on the active
    // mode: set in BYPASS (Mode A: analog path, DAC off), clear in DSP (Mode B:
    // DAC drives the outputs), so OR them in from s_mode.
    //
    // Write-only (no read-back): heavy live reads on the marginal MCLK-coupled bus
    // trip the watchdog. This is one re-runnable write, so es8388_write_reg's
    // NACK-retry is enough. PIN_PAM_SD hard-mutes the speaker amp on top of this.
    const uint8_t dac_pdn = (s_mode == ES8388_MODE_BYPASS) ? 0xc0 : 0x00;
    const uint8_t hp_bits = 0x30;   // LOUT1 + ROUT1
    const uint8_t ln_bits = 0x0c;   // LOUT2 + ROUT2
    uint8_t val;
    switch (out) {
    case ES8388_OUT_HEADPHONE: val = dac_pdn | hp_bits;           break;
    case ES8388_OUT_SPEAKER:   val = dac_pdn | ln_bits;           break;
    case ES8388_OUT_BOTH:
    default:                   val = dac_pdn | hp_bits | ln_bits; break;
    }
    ESP_LOGI(TAG, "route: %s (DACPOWER=0x%02x)",
             out == ES8388_OUT_HEADPHONE ? "headphone (HP amp)" :
             out == ES8388_OUT_SPEAKER   ? "speaker (line out)" : "both", val);
    return es8388_write_reg(ES8388_DACPOWER, val);
}

esp_err_t es8388_set_output_volume(int percent)
{
    // HP gets a lower ceiling than the line out so the wheel (Mode A) can't drive
    // headphones to a blasting level at full travel (see the ceiling #defines).
    uint8_t hp   = vol_to_reg_max(percent, ES8388_HP_VOL_MAX);
    uint8_t line = vol_to_reg_max(percent, ES8388_LINE_VOL_MAX);
    ESP_RETURN_ON_ERROR(es8388_write_reg(ES8388_DACCONTROL24, hp),   TAG, "HP L");
    ESP_RETURN_ON_ERROR(es8388_write_reg(ES8388_DACCONTROL25, hp),   TAG, "HP R");
    ESP_RETURN_ON_ERROR(es8388_write_reg(ES8388_DACCONTROL26, line), TAG, "line L");
    ESP_RETURN_ON_ERROR(es8388_write_reg(ES8388_DACCONTROL27, line), TAG, "line R");
    return ESP_OK;
}
