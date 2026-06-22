#include "buttons.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "pinmap.h"
#include "settings.h"

static const char *TAG = "buttons";

#define DEBOUNCE_MS         CONFIG_GBHIFI_DEBOUNCE_MS
// Sentinel posted into the GPIO queue when the hold deadline timer fires.
// Chosen outside the GPIO-number range so it can't collide with a real pin
// event.
#define HOLD_DEADLINE_EVT   0xFFFFFFFFu

// Hold-menu progress for the Connect/Pair button: 0 = held but not yet past the
// connect threshold, 1 = in the connect zone, 2 = in the pair zone, 3 = in the
// mode-toggle zone. Bumped as the deadline timer crosses each rung (emitting a
// feedback chime); the action fires on release for the current stage. Reset to 0
// on each fresh press. The absolute thresholds are snapshotted from settings at
// press time so a mid-hold settings edit can't desync the per-rung re-arm deltas.
static QueueHandle_t s_q          = NULL;
static TimerHandle_t s_cp_hold_t  = NULL;
static bool s_cp_pressed          = false;
static int  s_cp_stage            = 0;
static uint16_t s_hold_connect_ms = 3000;
static uint16_t s_hold_pair_ms    = 6000;
static uint16_t s_hold_mode_ms    = 9000;
static bool s_hp_plugged          = false;
static volatile bool s_emit_enabled = true;
static btn_event_cb_t s_event_cb  = NULL;

void buttons_set_event_cb(btn_event_cb_t cb)
{
    s_event_cb = cb;
}

void buttons_set_emit_enabled(bool enabled)
{
    s_emit_enabled = enabled;
}

void buttons_cancel_hold(void)
{
    if (s_cp_hold_t) xTimerStop(s_cp_hold_t, 0);
    s_cp_stage   = 0;
    s_cp_pressed = (gpio_get_level(PIN_CP_BUTTON) == 0);
}

static void emit(btn_event_t ev)
{
    if (s_event_cb && s_emit_enabled) {
        s_event_cb(ev);
    }
}

static void IRAM_ATTR gpio_isr(void *arg)
{
    uint32_t gpio = (uint32_t)arg;
    xQueueSendFromISR(s_q, &gpio, NULL);
}

static void cp_deadline_cb(TimerHandle_t t)
{
    uint32_t sentinel = HOLD_DEADLINE_EVT;
    xQueueSend(s_q, &sentinel, 0);
}

static void button_task(void *arg)
{
    // Capture boot state without emitting events: there's no prior level to
    // diff against, so any "edge" here is meaningless. Logged once. Note
    // inverted HP polarity: GBA grounds the detect line when unplugged.
    s_hp_plugged = (gpio_get_level(PIN_HP_DETECT) == 1);
    s_cp_pressed = (gpio_get_level(PIN_CP_BUTTON) == 0);
    ESP_LOGI(TAG, "boot state: hp_plugged=%d cp_pressed=%d",
             (int)s_hp_plugged, (int)s_cp_pressed);

    uint32_t evt;
    for (;;) {
        if (xQueueReceive(s_q, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (evt == HOLD_DEADLINE_EVT) {
            // A hold threshold elapsed. Bump one zone if still held and chime
            // the rung we just entered (feedback). The action fires on release,
            // not here. Re-arm the one-shot for the delta to the next rung
            // (ChangePeriod also restarts it).
            if (!s_cp_pressed) {
                continue;
            }
            if (s_cp_stage == 0) {
                ESP_LOGI(TAG, "CP zone: connect (held %u ms)", s_hold_connect_ms);
                s_cp_stage = 1;
                emit(BTN_EV_CP_ZONE_CONNECT);
                xTimerChangePeriod(s_cp_hold_t,
                                   pdMS_TO_TICKS(s_hold_pair_ms - s_hold_connect_ms), 0);
            } else if (s_cp_stage == 1) {
                ESP_LOGI(TAG, "CP zone: pair (held %u ms)", s_hold_pair_ms);
                s_cp_stage = 2;
                emit(BTN_EV_CP_ZONE_PAIR);
                xTimerChangePeriod(s_cp_hold_t,
                                   pdMS_TO_TICKS(s_hold_mode_ms - s_hold_pair_ms), 0);
            } else if (s_cp_stage == 2) {
                ESP_LOGI(TAG, "CP zone: mode (held %u ms)", s_hold_mode_ms);
                s_cp_stage = 3;
                emit(BTN_EV_CP_ZONE_MODE);
                // Top rung: don't re-arm; nothing escalates past mode-toggle.
            }
            continue;
        }

        // Debounce: ignore the edge for 20 ms, then resample the pin. If
        // the level matches the prior state we just rode out chatter from
        // a still-current press/release and have nothing to emit.
        vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
        int level = gpio_get_level((gpio_num_t)evt);

        if (evt == PIN_CP_BUTTON) {
            bool pressed = (level == 0);
            if (pressed == s_cp_pressed) {
                continue;
            }
            s_cp_pressed = pressed;
            if (pressed) {
                // Fresh press: snapshot the runtime-tunable thresholds and start
                // timing at the connect rung. ChangePeriod resets the period (a
                // prior press may have left it at a delta) and starts the timer.
                gbhifi_settings_t st;
                settings_get(&st);
                s_hold_connect_ms = st.hold_connect_ms;
                s_hold_pair_ms    = st.hold_pair_ms;
                s_hold_mode_ms    = st.hold_mode_ms;
                s_cp_stage = 0;
                xTimerChangePeriod(s_cp_hold_t, pdMS_TO_TICKS(s_hold_connect_ms), 0);
            } else {
                // Release: fire the action for the zone we ended up in.
                xTimerStop(s_cp_hold_t, 0);
                switch (s_cp_stage) {
                case 0: ESP_LOGI(TAG, "CP short");        emit(BTN_EV_CP_SHORT);   break;
                case 1: ESP_LOGI(TAG, "CP connect");      emit(BTN_EV_CP_CONNECT); break;
                case 2: ESP_LOGI(TAG, "CP pair");         emit(BTN_EV_CP_PAIR);    break;
                case 3: ESP_LOGI(TAG, "CP mode-toggle");  emit(BTN_EV_CP_MODE);    break;
                }
            }
        } else if (evt == PIN_HP_DETECT) {
            bool plugged = (level == 1);  // GBA pulls LOW when unplugged
            if (plugged == s_hp_plugged) {
                continue;
            }
            s_hp_plugged = plugged;
            ESP_LOGI(TAG, "HP %s", plugged ? "plug" : "unplug");
            emit(plugged ? BTN_EV_HP_PLUG : BTN_EV_HP_UNPLUG);
        }
    }
}

void buttons_refresh_gpio(void)
{
    // After an ext0/ext1 sleep wake (deep or light), GPIO34/35 may still be
    // routed through the RTC IO mux from the wake configuration. A plain
    // gpio_config() doesn't reclaim them; we have to explicitly release each
    // pin back to digital function or the edge interrupts never fire. Harmless
    // on a cold boot (the pins weren't in RTC mode). The already-installed ISR
    // handlers stay valid; only the pad/mux + interrupt type are refreshed.
    // (No gpio_hold_dis here: PIN_CP_BUTTON/PIN_HP_DETECT are input-only GPIOs,
    // which gpio_hold can't latch; rtc_gpio_deinit is what reclaims them.)
    rtc_gpio_deinit(PIN_CP_BUTTON);
    rtc_gpio_deinit(PIN_HP_DETECT);

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_CP_BUTTON) | (1ULL << PIN_HP_DETECT),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,    // external 10k pull-ups
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
}

esp_err_t buttons_init(void)
{
    buttons_refresh_gpio();

    s_q = xQueueCreate(16, sizeof(uint32_t));
    // Initial period is a placeholder; every press resets it via
    // xTimerChangePeriod() from the snapshotted settings thresholds.
    s_cp_hold_t = xTimerCreate("cp_hold", pdMS_TO_TICKS(3000),
                               pdFALSE, NULL, cp_deadline_cb);
    xTaskCreate(button_task, "buttons", 3072, NULL, 7, NULL);

    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_CP_BUTTON,
                                         gpio_isr, (void *)(uint32_t)PIN_CP_BUTTON));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_HP_DETECT,
                                         gpio_isr, (void *)(uint32_t)PIN_HP_DETECT));

    return ESP_OK;
}
