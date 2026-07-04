#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "sdkconfig.h"

#include "app_sm.h"
#include "audio_pipeline.h"
#include "ble_config.h"
#include "bt_a2d.h"
#include "buttons.h"
#include "console.h"
#include "dsp.h"
#include "es8388.h"
#include "fs.h"
#include "i2s_codec.h"
#include "pinmap.h"
#include "settings.h"
#include "sfx.h"

static const char *TAG = "gbhifi";

// Drain the ADC and feed the DAC one I2S block. Passed to the es8388 init/verify
// calls so the I2S DMA stays serviced during I2C config; an unserviced DMA storms
// the CPU and trips the watchdog. Runs before audio_pipeline_start(), so nothing
// else touches I2S yet.
static void codec_service_i2s(void)
{
    static int32_t buf[256 * 2];
    size_t br = 0, bw = 0;
    if (i2s_codec_read(buf, sizeof(buf), &br, 50) == ESP_OK && br) {
        i2s_codec_write(buf, br, &bw, 50);
    }
}

// Hold-to-wake gate. The Connect/Pair button is tapped off the GBA R shoulder
// net and is also an EXT1 deep-sleep wake source, so a gameplay tap would
// otherwise wake the chip and spin up BT. On an EXT1 wake, require the button
// held for CONFIG_GBHIFI_WAKE_HOLD_S; a shorter press re-arms the wake sources and
// returns to deep sleep before any heavy init runs. PIN_PAM_SD stays RTC-held
// across the bounce so the speaker state is preserved.
static void gate_cp_wake(void)
{
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT1) {
        return;  // cold boot or HP-detect (EXT0) wake
    }

    // Reclaim the button and HP pins from the RTC mux so we can read them.
    rtc_gpio_deinit(PIN_CP_BUTTON);
    rtc_gpio_deinit(PIN_HP_DETECT);
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << PIN_CP_BUTTON) | (1ULL << PIN_HP_DETECT),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,   // external 10k pull-ups
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in));

    ESP_LOGI(TAG, "Connect/Pair wake: hold %d s to confirm (else back to sleep)",
             CONFIG_GBHIFI_WAKE_HOLD_S);
    for (int i = 0; i < CONFIG_GBHIFI_WAKE_HOLD_S * 50; i++) {
        if (gpio_get_level(PIN_CP_BUTTON) != 0) {  // released: game input
            ESP_LOGI(TAG, "button released after %d ms, back to sleep", i * 20);
            // Re-arm the same wake sources as DEEP_IDLE entry: ext0 on HP at the
            // opposite of its current level, ext1 on R LOW. Keep PIN_PAM_SD held.
            int hp_wake_level = (gpio_get_level(PIN_HP_DETECT) == 1) ? 0 : 1;
            ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(PIN_HP_DETECT,
                                                         hp_wake_level));
            ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup(
                1ULL << PIN_CP_BUTTON, ESP_EXT1_WAKEUP_ALL_LOW));
            gpio_deep_sleep_hold_en();
            esp_deep_sleep_start();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGI(TAG, "button held, wake confirmed");
}

// Boot-time factory reset: if the Connect/Pair button (active LOW) is held from
// power-on for CONFIG_GBHIFI_FACTORY_RESET_S seconds, clear all bonded sinks.
// Releasing it before the deadline aborts. Runs after buttons_init and
// bt_a2d_init.
static void maybe_factory_reset(void)
{
    if (gpio_get_level(PIN_CP_BUTTON) != 0) {
        return;  // button not held at boot
    }
    ESP_LOGW(TAG, "Connect/Pair held at boot, keep holding %d s for factory reset",
             CONFIG_GBHIFI_FACTORY_RESET_S);
    for (int i = 0; i < CONFIG_GBHIFI_FACTORY_RESET_S * 10; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (gpio_get_level(PIN_CP_BUTTON) != 0) {
            ESP_LOGI(TAG, "button released, factory reset aborted");
            return;
        }
    }
    ESP_LOGW(TAG, "factory reset: clearing all bonds");
    bt_a2d_clear_bonds();
}

// Anti-brick confirmation for OTA. A freshly OTA'd image boots in PENDING_VERIFY;
// reaching this point means init completed without a panic, so mark the image
// good and cancel the pending rollback. A bad image that panics during init
// never gets here and the bootloader reverts on the next reset. The gate is
// "init completed" rather than a runtime milestone because the mod sits on the
// GBA switched rail and power-cycles constantly. Cable-flashed images boot
// UNDEFINED (rollback not armed), so this is a no-op for them.
static void ota_confirm_image_valid(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (!run || esp_ota_get_state_partition(run, &state) != ESP_OK) return;
    if (state != ESP_OTA_IMG_PENDING_VERIFY) return;
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        ESP_LOGW(TAG, "OTA image on %s confirmed good, rollback cancelled", run->label);
    } else {
        ESP_LOGE(TAG, "esp_ota_mark_app_valid_cancel_rollback failed");
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "GameBoy HiFi v%s booting", esp_app_get_description()->version);

    // Log the wake cause so the UART trace shows cold boot vs HP-plug (ext0) vs
    // R-button (ext1). The boot path is the same in all cases.
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    switch (cause) {
    case ESP_SLEEP_WAKEUP_EXT0: {
        // EXT0 polarity is set at sleep entry to the opposite of the HP level,
        // so on wake the line is at the level that triggered it.
        int lvl = gpio_get_level(PIN_HP_DETECT);
        ESP_LOGI(TAG, "wake cause: EXT0 (HP %s, GPIO35 now %s)",
                 lvl ? "plug" : "unplug", lvl ? "HIGH" : "LOW");
        break;
    }
    case ESP_SLEEP_WAKEUP_EXT1: {
        uint64_t mask = esp_sleep_get_ext1_wakeup_status();
        ESP_LOGI(TAG, "wake cause: EXT1 (mask=0x%" PRIx64 ", R-button)", mask);
        break;
    }
    default:
        ESP_LOGI(TAG, "wake cause: cold boot (reason %d)", (int)cause);
        break;
    }

    // On an EXT1 wake, confirm an intentional hold before expensive init.
    gate_cp_wake();

    // Boot-mute the speaker amp. Its SD divider floats to "enabled", so without
    // this the amp runs before the DAC is clocking valid samples and pops. Cold
    // boot only: on a deep-sleep wake the pin is still RTC-held from before sleep,
    // and app_sm releases the hold and re-applies the mute policy.
    if (cause == ESP_SLEEP_WAKEUP_UNDEFINED) {
        gpio_config_t amp_sd = {
            .pin_bit_mask = 1ULL << PIN_PAM_SD,
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&amp_sd));
        gpio_set_level(PIN_PAM_SD, 0);
    }

    // NVS holds the BT controller PHY calibration and the Bluedroid bond keys.
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs);

    // Bring up the I2S master bus first so the ES8388 sees MCLK before its I2C
    // config runs (the codec won't start its state machine without MCLK). Settle,
    // configure over I2C, then verify-and-fix any writes corrupted by MCLK
    // coupling on the bus.
    ESP_ERROR_CHECK(i2s_codec_init());
    ESP_ERROR_CHECK(i2s_codec_start());
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_ERROR_CHECK(es8388_init(codec_service_i2s));
    es8388_verify_config(codec_service_i2s);   // not fatal if it cannot converge

    // DSP subsystem, ready before the pipeline task calls into it: settings holds
    // the parameter snapshot, fs mounts the clip store, sfx owns the cue feeder,
    // dsp seeds its coefficients from settings.
    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(fs_init());
    ESP_ERROR_CHECK(sfx_init());
    ESP_ERROR_CHECK(dsp_init());

    ESP_ERROR_CHECK(audio_pipeline_start());

    ESP_ERROR_CHECK(buttons_init());

    ESP_ERROR_CHECK(bt_a2d_init());

#if CONFIG_GBHIFI_BLE_CONFIG
    // BLE GATT config server alongside the A2DP source (controller in BTDM dual
    // mode). Comes up after bt_a2d_init() since Bluedroid is enabled there.
    ESP_ERROR_CHECK(ble_config_init());
#endif

    // Factory reset before the state machine starts: Connect/Pair held from
    // power-on clears all bonds, dropping the next boot into pairing.
    maybe_factory_reset();

    ESP_ERROR_CHECK(app_sm_start());

    // UART REPL control surface, started last. The web UI (BLE config) drives the
    // same settings_* API. Needs an interactive serial monitor for input.
    ESP_ERROR_CHECK(console_start());

    // Init reached without a panic: confirm a pending OTA image good.
    ota_confirm_image_valid();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        ESP_LOGI(TAG, "alive (free heap: %" PRIu32 " B, min ever: %" PRIu32 " B)",
                 esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
        app_sm_batt_check();
    }
}
