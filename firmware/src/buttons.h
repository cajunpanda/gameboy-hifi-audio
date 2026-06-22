#pragma once

#include "esp_err.h"

// Connect/Pair button (GPIO34, active-LOW) and headphone-detect (GPIO35,
// active-HIGH, inverted-polarity sense line, see pinmap.h) inputs with
// edge-triggered interrupts and a 20 ms debouncer task.
//
// The Connect/Pair button drives a chime-guided hold menu. As the hold passes
// each threshold the device chimes that rung (BTN_EV_CP_ZONE_*); the action
// fires on release for whichever zone the press is released in (so over-holding
// past a rung doesn't trigger the earlier ones). Thresholds are runtime-tunable
// (settings.hold_*):
//   release < connect           CP_SHORT  (benign tap, mashed during gameplay)
//   release in [connect, pair)  CP_CONNECT (reconnect bonded headphones)
//   release in [pair, mode)     CP_PAIR    (pair a new device)
//   release >= mode             CP_MODE    (toggle Mode A / B)
// In Mode A the menu can't chime (DSP/SFX stopped in bypass); app_sm's
// light-sleep loop times a plain hold to exit instead.

typedef enum {
    BTN_EV_CP_SHORT,        // released before the connect zone (benign tap)
    BTN_EV_CP_CONNECT,      // released in the connect zone
    BTN_EV_CP_PAIR,         // released in the pair zone
    BTN_EV_CP_MODE,         // released in the mode-toggle zone
    BTN_EV_CP_ZONE_CONNECT, // hold crossed into the connect zone (feedback chime)
    BTN_EV_CP_ZONE_PAIR,    // hold crossed into the pair zone (feedback chime)
    BTN_EV_CP_ZONE_MODE,    // hold crossed into the mode-toggle zone (feedback chime)
    BTN_EV_HP_PLUG,         // headphone plug inserted
    BTN_EV_HP_UNPLUG,       // headphone plug removed
} btn_event_t;

typedef void (*btn_event_cb_t)(btn_event_t ev);

// Subscribe to button events. Callback fires on the debouncer task; must
// not block for long. Only one subscriber; second call replaces the first.
// May be called before or after buttons_init().
void buttons_set_event_cb(btn_event_cb_t cb);

esp_err_t buttons_init(void);

// Re-establish the digital input + edge-interrupt config on the Connect/Pair
// and HP-detect pins. Call after a light-sleep wake that armed ext0/ext1 on
// them (Mode A's duty loop), which can leave the pads routed through the RTC IO
// mux so the normal edge ISRs stop firing. Reuses the already-installed ISR
// handlers + task/queue; only the pad/mux + interrupt type are refreshed.
void buttons_refresh_gpio(void);

// Gate event emission. While disabled, the debouncer still tracks press/HP
// state but emits nothing. app_sm suspends emission for the whole Mode A
// session (its light-sleep loop reads the R button + HP directly), so the
// debouncer can't race a stray release event past Mode A's exit and re-toggle
// the mode. Re-enable on exit, after buttons_cancel_hold().
void buttons_set_emit_enabled(bool enabled);

// Reset the in-progress Connect/Pair hold: stop the deadline timer, clear the
// zone stage, and resync the pressed flag to the live pin level. Called on Mode
// A exit so a hold that was timed (silently) during the suspended window can't
// fire an action once emission is re-enabled.
void buttons_cancel_hold(void);
