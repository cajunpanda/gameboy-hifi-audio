#pragma once

#include "esp_err.h"

// Serial-console control surface for the DSP settings. A UART REPL whose
// commands wrap the settings_* / sfx_* APIs; it holds no state of its own.
// Coexists with ESP_LOG on UART0, but needs an interactive monitor to accept
// input. Start after app_sm.
esp_err_t console_start(void);

