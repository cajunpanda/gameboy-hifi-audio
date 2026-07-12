#include "console.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "linenoise/linenoise.h"

#include "app_sm.h"
#include "audio_pipeline.h"
#include "ble_config.h"
#include "bt_a2d.h"
#include "es8388.h"
#include "fs.h"
#include "settings.h"
#include "sfx.h"

static const char *TAG = "console";

// `vol <0-100>`: set digital speaker volume.
static int cmd_vol(int argc, char **argv)
{
    if (argc != 2) { printf("usage: vol <0-100>\n"); return 1; }
    settings_set_volume((uint8_t)atoi(argv[1]));
    return 0;
}

// `eq on|off [bass mid treble]`: toggle speaker EQ, optionally set the three
// band gains in dB (-12..+12). Toggling without gains keeps the current gains.
static int cmd_eq(int argc, char **argv)
{
    if (argc < 2) { printf("usage: eq on|off [bass mid treble dB]\n"); return 1; }
    bool on = (strcmp(argv[1], "on") == 0);
    gbhifi_settings_t s;
    settings_get(&s);
    int8_t b = s.eq_bass_db, m = s.eq_mid_db, t = s.eq_treble_db;
    if (argc >= 5) {
        b = (int8_t)atoi(argv[2]);
        m = (int8_t)atoi(argv[3]);
        t = (int8_t)atoi(argv[4]);
    }
    settings_set_eq(on, b, m, t);
    return 0;
}

// `eqhp on|off [bass mid treble]`: headphone EQ profile, used while wired HP is
// plugged; independent of the speaker EQ. Same 3 bands/freqs.
static int cmd_eqhp(int argc, char **argv)
{
    if (argc < 2) { printf("usage: eqhp on|off [bass mid treble dB]\n"); return 1; }
    bool on = (strcmp(argv[1], "on") == 0);
    gbhifi_settings_t s;
    settings_get(&s);
    int8_t b = s.eq_hp_bass_db, m = s.eq_hp_mid_db, t = s.eq_hp_treble_db;
    if (argc >= 5) {
        b = (int8_t)atoi(argv[2]);
        m = (int8_t)atoi(argv[3]);
        t = (int8_t)atoi(argv[4]);
    }
    settings_set_hp_eq(on, b, m, t);
    return 0;
}

// `volbt <0-100>`: Bluetooth digital volume, independent of speaker volume.
static int cmd_volbt(int argc, char **argv)
{
    if (argc != 2) { printf("usage: volbt <0-100>\n"); return 1; }
    settings_set_bt_volume((uint8_t)atoi(argv[1]));
    return 0;
}

// `eqbt on|off [bass mid treble]`: independent EQ on the Bluetooth path.
// Toggling without gains keeps the current gains.
static int cmd_eqbt(int argc, char **argv)
{
    if (argc < 2) { printf("usage: eqbt on|off [bass mid treble dB]\n"); return 1; }
    bool on = (strcmp(argv[1], "on") == 0);
    gbhifi_settings_t s;
    settings_get(&s);
    int8_t b = s.eq_bt_bass_db, m = s.eq_bt_mid_db, t = s.eq_bt_treble_db;
    if (argc >= 5) {
        b = (int8_t)atoi(argv[2]);
        m = (int8_t)atoi(argv[3]);
        t = (int8_t)atoi(argv[4]);
    }
    settings_set_bt_eq(on, b, m, t);
    return 0;
}

// `sfx on|off [level_dB]`: enable/disable cue mixing, optional level trim.
static int cmd_sfx(int argc, char **argv)
{
    if (argc < 2) { printf("usage: sfx on|off [level_dB]\n"); return 1; }
    bool on = (strcmp(argv[1], "on") == 0);
    gbhifi_settings_t s;
    settings_get(&s);
    int8_t lvl = (argc >= 3) ? (int8_t)atoi(argv[2]) : s.sfx_level_db;
    settings_set_sfx(on, lvl);
    return 0;
}

// `chime [pairing|connect|disconnect]`: fire a synth cue (default connect).
static int cmd_chime(int argc, char **argv)
{
    synth_id_t id = SFX_SYNTH_CONNECT;
    if (argc >= 2) {
        if      (strcmp(argv[1], "pairing")    == 0) id = SFX_SYNTH_PAIRING;
        else if (strcmp(argv[1], "connect")    == 0) id = SFX_SYNTH_CONNECT;
        else if (strcmp(argv[1], "disconnect") == 0) id = SFX_SYNTH_DISCONNECT;
    }
    sfx_trigger_synth(id);
    return 0;
}

// `play <name>`: play /clips/<name>.gsfx.
static int cmd_play(int argc, char **argv)
{
    if (argc != 2) { printf("usage: play <name>\n"); return 1; }
    sfx_trigger_clip(argv[1]);
    return 0;
}

// `ls`: list the clip store.
static int cmd_ls(int argc, char **argv)
{
    (void)argc; (void)argv;
    DIR *d = opendir(FS_CLIPS_MOUNT);
    if (!d) { printf("cannot open %s\n", FS_CLIPS_MOUNT); return 1; }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        printf("  %s\n", e->d_name);
    }
    closedir(d);
    return 0;
}

// `save`: persist current settings to NVS.
static int cmd_save(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("%s\n", settings_commit() == ESP_OK ? "saved" : "save FAILED");
    return 0;
}

// `out2 <reg>`: live-tune the ES8388 LOUT2/ROUT2 (speaker line-out) driver
// volume for setting the PAM8302A level on a scope. <reg> is the raw 6-bit
// DACCONTROL26/27 value 0x00..0x21 (accepts decimal or 0x-hex); dB = reg*1.5-45
// (range -45..+4.5 dB). Pokes the registers directly; does not persist.
#define ES8388_LOUT2VOL 0x30  // DACCONTROL26
#define ES8388_ROUT2VOL 0x31  // DACCONTROL27
static int cmd_out2(int argc, char **argv)
{
    if (argc != 2) { printf("usage: out2 <0x00-0x21>  (dB = reg*1.5-45)\n"); return 1; }
    long v = strtol(argv[1], NULL, 0);
    if (v < 0 || v > 0x21) { printf("out of range: reg must be 0x00..0x21\n"); return 1; }
    if (es8388_write_reg(ES8388_LOUT2VOL, (uint8_t)v) != ESP_OK ||
        es8388_write_reg(ES8388_ROUT2VOL, (uint8_t)v) != ESP_OK) {
        printf("I2C write failed\n");
        return 1;
    }
    int tenths = (int)v * 15 - 450;  // dB*10, step 1.5 dB so always .0/.5
    printf("LOUT2/ROUT2 = 0x%02x  (%d.%d dB)\n",
           (unsigned)v, tenths / 10, abs(tenths % 10));
    return 0;
}

// `out1 <reg>`: live-tune the ES8388 LOUT1/ROUT1 (headphone amp) driver volume.
// Same 6-bit DACCONTROL24/25 field as out2 (0x00..0x21; dB = reg*1.5-45, range
// -45..+4.5 dB). Headphones want roughly -18..-30 dB below line. Pokes the
// registers directly; does not persist.
#define ES8388_LOUT1VOL 0x2e  // DACCONTROL24
#define ES8388_ROUT1VOL 0x2f  // DACCONTROL25
static int cmd_out1(int argc, char **argv)
{
    if (argc != 2) { printf("usage: out1 <0x00-0x21>  (dB = reg*1.5-45)\n"); return 1; }
    long v = strtol(argv[1], NULL, 0);
    if (v < 0 || v > 0x21) { printf("out of range: reg must be 0x00..0x21\n"); return 1; }
    if (es8388_write_reg(ES8388_LOUT1VOL, (uint8_t)v) != ESP_OK ||
        es8388_write_reg(ES8388_ROUT1VOL, (uint8_t)v) != ESP_OK) {
        printf("I2C write failed\n");
        return 1;
    }
    int tenths = (int)v * 15 - 450;  // dB*10, step 1.5 dB so always .0/.5
    printf("LOUT1/ROUT1 = 0x%02x  (%d.%d dB)\n",
           (unsigned)v, tenths / 10, abs(tenths % 10));
    return 0;
}

// `outvol <0-100>`: Mode A analog volume: set all four ES8388 output drivers
// (HP LOUT1/ROUT1 + speaker LOUT2/ROUT2) together. In Mode A (analog bypass) the
// DSP `vol` does nothing; this is the codec-side output volume the VOL wheel
// drives. Overrides the `out2` speaker calibration while in use (it also writes
// LOUT2/ROUT2). Works in Mode B too (trims the output drivers).
static int cmd_outvol(int argc, char **argv)
{
    if (argc != 2) { printf("usage: outvol <0-100>\n"); return 1; }
    int pct = atoi(argv[1]);
    if (pct < 0 || pct > 100) { printf("out of range: 0-100\n"); return 1; }
    printf("%s\n", es8388_set_output_volume(pct) == ESP_OK
                   ? "output drivers set" : "FAILED (check log)");
    return 0;
}

// `mode a|b`: set the sticky operating mode. Sets the settings.mode_a
// preference; app_sm picks it up on its ~200 ms poll and runs the full
// transition (codec bypass/DSP + stop/restart the pipeline and I2S +
// BT/route/amp handling). Persist with `save`. For a raw codec-only poke
// without the orchestration, use `codecmode`.
// `bt connect|pair`: drive the BT state machine from the console -- posts the
// same events a Connect/Pair button hold releases, so bench tests can rebond or
// re-page a sink without touching the physical button. (To silence the radio
// instead, that's `radio off`.)
static int cmd_bt(int argc, char **argv)
{
    if (argc != 2) { printf("usage: bt connect|pair\n"); return 1; }
    if (strcmp(argv[1], "connect") == 0) {
        app_sm_request_bt_connect();
        printf("bt: connect requested (pages bonded sinks)\n");
    } else if (strcmp(argv[1], "pair") == 0) {
        app_sm_request_bt_pair();
        printf("bt: pairing requested (inquiry for a new sink)\n");
    } else {
        printf("usage: bt connect|pair\n"); return 1;
    }
    return 0;
}

static int cmd_mode(int argc, char **argv)
{
    if (argc != 2) { printf("usage: mode a|b  (a=analog bypass/battery, b=DSP)\n"); return 1; }
    char m = argv[1][0];
    // Mode changes reboot into the target mode (Mode A boots BT-less for power +
    // reliability; Mode B boots with the radio). app_sm_switch_mode() persists and
    // restarts, so this does not return unless we're already in that mode.
    if (m == 'a' || m == 'A') {
        printf("Mode A: saving + rebooting into analog bypass...\n");
        app_sm_switch_mode(true);
        printf("already in Mode A\n");
    } else if (m == 'b' || m == 'B') {
        printf("Mode B: saving + rebooting into DSP...\n");
        app_sm_switch_mode(false);
        printf("already in Mode B\n");
    } else {
        printf("usage: mode a|b\n"); return 1;
    }
    return 0;
}

// `codecmode a|b`: raw bench tool: reconfigure only the codec (es8388_set_mode)
// without the app_sm orchestration (no pipeline stop / routing / BT handling).
// Use it to debug the codec path in isolation; `mode` is the real control.
static int cmd_codecmode(int argc, char **argv)
{
    if (argc != 2) { printf("usage: codecmode a|b  (raw codec poke)\n"); return 1; }
    char m = argv[1][0];
    if (m == 'a' || m == 'A') {
        printf("%s\n", es8388_set_mode(ES8388_MODE_BYPASS) == ESP_OK
                       ? "codec: analog bypass" : "FAILED (check log)");
    } else if (m == 'b' || m == 'B') {
        printf("%s\n", es8388_set_mode(ES8388_MODE_DSP) == ESP_OK
                       ? "codec: DSP" : "FAILED (check log)");
    } else {
        printf("usage: codecmode a|b\n"); return 1;
    }
    return 0;
}

// `bootmode on|off`: whether the Mode A/B preference persists across a power
// cycle. on (default) = boot back into Mode A if powered off in Mode A; off =
// always boot Mode B local. Persist with `save`.
static int cmd_bootmode(int argc, char **argv)
{
    if (argc != 2) { printf("usage: bootmode on|off\n"); return 1; }
    if (strcmp(argv[1], "on") == 0) {
        settings_set_boot_mode_a(true);
        printf("boot_mode_a = on (persist Mode A across power cycles)\n");
    } else if (strcmp(argv[1], "off") == 0) {
        settings_set_boot_mode_a(false);
        printf("boot_mode_a = off (always boot Mode B local)\n");
    } else {
        printf("usage: bootmode on|off\n"); return 1;
    }
    return 0;
}


// `autoconnect on|off`: Bluetooth connect-on-boot policy. off (default) = manual:
// on boot the radio comes up idle and the device waits in LOCAL_ONLY; nothing
// pages/inquires until a Connect/Pair (R) hold. on = auto: re-page bonded sinks
// (or pair if none) on boot. Takes effect at the next boot. Persist with `save`.
static int cmd_autoconnect(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "on") && strcmp(argv[1], "off"))) {
        printf("usage: autoconnect on|off\n"); return 1;
    }
    bool on = (strcmp(argv[1], "on") == 0);
    settings_set_auto_connect(on);
    printf("auto-connect %s (takes effect next boot; `save` to persist)\n",
           on ? "ON (page/pair on boot)" : "OFF (wait for Connect/Pair hold)");
    return 0;
}

// `wheel [on|off]`: with no arg, print the live VOL-wheel reading (GBA VR2
// wiper on GPIO39, VCC-referenced). With on|off, enable/disable the wheel
// driving volume. Default on (the wheel is the volume control); `wheel off`
// hands volume back to the console `vol` for bench work or when the wheel is
// unplugged. Mode B drives speaker DSP volume; Mode A drives codec output drivers.
static int cmd_wheel(int argc, char **argv)
{
    if (argc == 1) {
        int raw = -1, pct = -1;
        app_sm_vol_wheel_read(&raw, &pct);
        if (raw < 0) printf("VOL wheel: ADC unavailable\n");
        else printf("VOL wheel: raw=%d / 4095  pct=%d%%  (VBAT=%d mV)\n",
                    raw, pct, app_sm_read_vbat_mv());
        return 0;
    }
    if (argc != 2) { printf("usage: wheel [on|off]\n"); return 1; }
    bool on = (strcmp(argv[1], "on") == 0);
    if (!on && strcmp(argv[1], "off") != 0) { printf("usage: wheel [on|off]\n"); return 1; }
    app_sm_set_wheel_enabled(on);
    printf("VOL wheel %s\n", on ? "enabled" : "disabled");
    return 0;
}

// `sleep`: force an immediate DEEP_IDLE (deep sleep) for wake-reliability
// testing. The chip reboots on wake; wake via the R-button (EXT1, held
// CONFIG_GBHIFI_WAKE_HOLD_S) or an HP-detect edge (EXT0).
static int cmd_sleep(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("forcing DEEP_IDLE - wake via R-button hold or HP-detect edge\n");
    app_sm_request_sleep();
    return 0;
}

// `hp plug|unplug|follow`: bench override of HP-detect so a current measurement
// doesn't require physically holding the jack. `plug` forces plugged (speaker amp
// muted, audio routes to the HP amp), `unplug` forces unplugged (speaker path),
// `follow` releases the override back to the real GPIO.
static int cmd_hp(int argc, char **argv)
{
    if (argc != 2) { printf("usage: hp plug|unplug|follow\n"); return 1; }
    if      (strcmp(argv[1], "plug")   == 0) { app_sm_force_hp(1);  printf("HP forced PLUGGED\n"); }
    else if (strcmp(argv[1], "unplug") == 0) { app_sm_force_hp(0);  printf("HP forced UNPLUGGED\n"); }
    else if (strcmp(argv[1], "follow") == 0) { app_sm_force_hp(-1); printf("HP follows GPIO\n"); }
    else { printf("usage: hp plug|unplug|follow\n"); return 1; }
    return 0;
}

// `nr`: tunable noise reduction on the local capture path. With no args, show the
// current filters; otherwise set one (0 Hz = off). Find the tones to target with
// `wavedump` + tools/plot_wavedump.py. Persist with `save`.
//   nr                  show current filters
//   nr off              all filters off
//   nr hpf <hz>         high-pass corner (low-frequency hum)
//   nr lpf <hz>         low-pass corner (high-frequency hiss)
//   nr notch <hz> [q]   notch a discrete tone (e.g. the aliased PWM whine)
//   nr gate <dBFS> [range]  downward expander: fade the floor when below threshold
static int cmd_nr(int argc, char **argv)
{
    gbhifi_settings_t s;
    settings_get(&s);
    if (argc == 1) {
        printf("nr: hpf=%u Hz  lpf=%u Hz  notch=%u Hz (Q=%u)   (0 = off)\n",
               s.nr_hpf_hz, s.nr_lpf_hz, s.nr_notch_hz, s.nr_notch_q);
        printf("nr gate: thresh=%d dBFS  range=%d dB%s\n",
               s.nr_gate_thresh_db, s.nr_gate_range_db, s.nr_gate_thresh_db < 0 ? "" : "  (off)");
        return 0;
    }
    if (strcmp(argv[1], "gate") == 0) {
        int8_t  th = s.nr_gate_thresh_db;
        uint8_t rg = s.nr_gate_range_db;
        if      (argc >= 3 && strcmp(argv[2], "off") == 0) th = 0;
        else if (argc >= 3) { th = (int8_t)atoi(argv[2]); if (argc >= 4) rg = (uint8_t)atoi(argv[3]); }
        else { printf("usage: nr gate <thresh_dBFS> [range_dB] | nr gate off\n"); return 1; }
        settings_set_nr_gate(th, rg);
        settings_get(&s);
        printf("nr gate: thresh=%d dBFS  range=%d dB%s\n",
               s.nr_gate_thresh_db, s.nr_gate_range_db, s.nr_gate_thresh_db < 0 ? "" : "  (off)");
        return 0;
    }
    uint16_t hpf = s.nr_hpf_hz, lpf = s.nr_lpf_hz, notch = s.nr_notch_hz;
    uint8_t  q   = s.nr_notch_q;
    if      (strcmp(argv[1], "off")   == 0) { hpf = lpf = notch = 0; settings_set_nr_gate(0, s.nr_gate_range_db); }
    else if (strcmp(argv[1], "hpf")   == 0 && argc >= 3) hpf   = (uint16_t)atoi(argv[2]);
    else if (strcmp(argv[1], "lpf")   == 0 && argc >= 3) lpf   = (uint16_t)atoi(argv[2]);
    else if (strcmp(argv[1], "notch") == 0 && argc >= 3) {
        notch = (uint16_t)atoi(argv[2]);
        if (argc >= 4) q = (uint8_t)atoi(argv[3]);
    } else {
        printf("usage: nr | nr off | nr hpf <hz> | nr lpf <hz> | nr notch <hz> [q] | nr gate <dBFS> [range]\n");
        return 1;
    }
    settings_set_nr(hpf, lpf, notch, q);
    printf("nr: hpf=%u Hz  lpf=%u Hz  notch=%u Hz (Q=%u)\n", hpf, lpf, notch, q);
    return 0;
}

// `radio on|off`: silence the radio (BT inquiry/paging + BLE advertising) for a
// clean GBA-only noise measurement; on restores normal operation.
static int cmd_radio(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "on") && strcmp(argv[1], "off"))) {
        printf("usage: radio on|off\n"); return 1;
    }
    bool on = (strcmp(argv[1], "on") == 0);
    bt_a2d_set_radio(on);
    ble_config_set_advertising(on);
    printf("radio %s\n", on ? "ON" : "OFF (BT inquiry + BLE advertising stopped)");
    return 0;
}

// `wavedump [n]`: capture n raw ADC frames (default 1024) and dump as CSV for
// tools/plot_wavedump.py. Pair with `radio off` for a clean GBA-only noise floor.
static int cmd_wavedump(int argc, char **argv)
{
    int n = (argc >= 2) ? atoi(argv[1]) : 0;   // 0 -> default (max) window
    audio_pipeline_capture(n);
    printf("wavedump requested (%s samples); dump follows\n",
           (argc >= 2) ? argv[1] : "1024");
    return 0;
}

// `batt`: read the battery rail (VBAT via the ADC1_CH0 sense divider).
static int cmd_batt(int argc, char **argv)
{
    (void)argc; (void)argv;
    int mv = app_sm_read_vbat_mv();
    if (mv < 0) { printf("battery: sense unavailable\n"); return 1; }
    printf("battery: VBAT=%d mV  (~%d%%)\n", mv, app_sm_batt_pct(mv));
    return 0;
}

// `hold [connect pair mode exit]`: show or set the R-button hold-menu
// thresholds in ms. With no args, print them; with 4 args, set
// connect/pair/mode/mode-exit (clamped + re-ordered so connect<pair<mode).
// mode-exit is the plain hold to leave Mode A. Persist with `save`.
static int cmd_hold(int argc, char **argv)
{
    gbhifi_settings_t s;
    if (argc == 1) {
        settings_get(&s);
        printf("hold (ms): connect=%u pair=%u mode=%u exit=%u\n",
               s.hold_connect_ms, s.hold_pair_ms, s.hold_mode_ms, s.hold_mode_exit_ms);
        return 0;
    }
    if (argc != 5) { printf("usage: hold [connect pair mode exit] (ms)\n"); return 1; }
    settings_set_hold_timings((uint16_t)atoi(argv[1]), (uint16_t)atoi(argv[2]),
                              (uint16_t)atoi(argv[3]), (uint16_t)atoi(argv[4]));
    settings_get(&s);
    printf("hold set: connect=%u pair=%u mode=%u exit=%u\n",
           s.hold_connect_ms, s.hold_pair_ms, s.hold_mode_ms, s.hold_mode_exit_ms);
    return 0;
}

// `get`: dump current settings.
static int cmd_get(int argc, char **argv)
{
    (void)argc; (void)argv;
    gbhifi_settings_t s;
    settings_get(&s);
    printf("vol: spk=%u%%  BT=%u%%\n", s.speaker_vol_pct, s.bt_vol_pct);
    printf("spk EQ=%s [bass %+d  mid %+d  treble %+d dB]\n",
           s.eq_enabled ? "on" : "off",
           s.eq_bass_db, s.eq_mid_db, s.eq_treble_db);
    printf("HP  EQ=%s [bass %+d  mid %+d  treble %+d dB]\n",
           s.eq_hp_enabled ? "on" : "off",
           s.eq_hp_bass_db, s.eq_hp_mid_db, s.eq_hp_treble_db);
    printf("BT  EQ=%s [bass %+d  mid %+d  treble %+d dB]\n",
           s.eq_bt_enabled ? "on" : "off",
           s.eq_bt_bass_db, s.eq_bt_mid_db, s.eq_bt_treble_db);
    printf("sfx=%s [%+d dB]\n", s.sfx_enabled ? "on" : "off", s.sfx_level_db);
    printf("mode=%c  boot_mode_a=%s  auto_connect=%s\n",
           s.mode_a ? 'A' : 'B', s.boot_mode_a ? "on" : "off",
           s.auto_connect ? "on" : "off");
    printf("hold (ms): connect=%u pair=%u mode=%u exit=%u\n",
           s.hold_connect_ms, s.hold_pair_ms, s.hold_mode_ms, s.hold_mode_exit_ms);
    return 0;
}

// `heap`: free-heap readout for debugging RAM pressure (e.g. BLE + A2DP
// coexistence). Internal is the DMA-capable pool the BT stack allocates from.
static int cmd_heap(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("heap: free=%u internal=%u largest_internal=%u min_ever=%u\n",
           (unsigned)esp_get_free_heap_size(),
           (unsigned)esp_get_free_internal_heap_size(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned)esp_get_minimum_free_heap_size());
    return 0;
}

static void register_cmds(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "vol",   .help = "Set speaker volume: vol <0-100>",          .func = cmd_vol },
        { .command = "volbt", .help = "Set Bluetooth volume: volbt <0-100>",      .func = cmd_volbt },
        { .command = "eq",    .help = "Speaker EQ: eq on|off [bass mid treble]",  .func = cmd_eq },
        { .command = "eqhp",  .help = "Headphone EQ: eqhp on|off [bass mid treble]", .func = cmd_eqhp },
        { .command = "eqbt",  .help = "Bluetooth EQ: eqbt on|off [bass mid treble]", .func = cmd_eqbt },
        { .command = "sfx",   .help = "Cue mixing: sfx on|off [level_dB]",        .func = cmd_sfx },
        { .command = "chime", .help = "Play a synth cue: chime [pairing|connect|disconnect]", .func = cmd_chime },
        { .command = "play",  .help = "Play a clip: play <name>",                .func = cmd_play },
        { .command = "out1",  .help = "Tune headphone-amp level: out1 <0x00-0x21>", .func = cmd_out1 },
        { .command = "out2",  .help = "Tune speaker line-out level: out2 <0x00-0x21>", .func = cmd_out2 },
        { .command = "bt",    .help = "Drive BT like the button: bt connect|pair", .func = cmd_bt },
        { .command = "mode",  .help = "Set operating mode (sticky): mode a|b (a=bypass/battery, b=DSP)", .func = cmd_mode },
        { .command = "codecmode", .help = "Raw codec poke (no orchestration): codecmode a|b", .func = cmd_codecmode },
        { .command = "bootmode", .help = "Persist Mode A across power cycles: bootmode on|off", .func = cmd_bootmode },
        { .command = "autoconnect", .help = "BT connect-on-boot: autoconnect on|off (off=wait for R hold)", .func = cmd_autoconnect },
        { .command = "hold",  .help = "R-button hold thresholds (ms): hold [connect pair mode exit]", .func = cmd_hold },
        { .command = "wheel", .help = "VOL wheel drives volume: wheel on|off",  .func = cmd_wheel },
        { .command = "batt",  .help = "Read battery rail voltage: batt",         .func = cmd_batt },
        { .command = "nr",    .help = "Noise reduction: nr | hpf|lpf|notch <hz> [q] | gate <dBFS> [range]", .func = cmd_nr },
        { .command = "radio", .help = "Silence radio for noise test: radio on|off", .func = cmd_radio },
        { .command = "wavedump",.help = "Capture ADC samples for analysis: wavedump [n]", .func = cmd_wavedump },
        { .command = "outvol",.help = "Mode A analog volume (HP+spk drivers): outvol <0-100>", .func = cmd_outvol },
        { .command = "hp",    .help = "Override HP-detect (bench): hp plug|unplug|follow", .func = cmd_hp },
        { .command = "sleep", .help = "Force deep sleep (wake-reliability test): sleep", .func = cmd_sleep },
        { .command = "ls",    .help = "List the clip store",                     .func = cmd_ls },
        { .command = "save",  .help = "Persist settings to NVS",                 .func = cmd_save },
        { .command = "get",   .help = "Show current settings",                   .func = cmd_get },
        { .command = "heap",  .help = "Show free heap (debug RAM pressure)",     .func = cmd_heap },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
}

esp_err_t console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "hifi>";
    repl_cfg.max_cmdline_length = 128;

    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(
        esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl),
        TAG, "repl_uart");

    esp_console_register_help_command();
    register_cmds();

    // With no real TTY, IDF's linenoise runs in "dumb" mode where an empty/EOF
    // read returns an empty line (allow_empty defaults true). When the serial
    // host detaches, stdin goes to continuous EOF and the REPL loop spins,
    // flooding the UART with blank lines, which starves the CPU and garbles the
    // lower-priority BT SBC/TX path. Disabling allow_empty makes an empty/EOF
    // read return NULL, so the REPL task exits cleanly on detach. Trade-off:
    // pressing Enter on an empty line also exits the console (reset to get it
    // back), fine for a bench tool.
    linenoiseAllowEmpty(false);

    ESP_RETURN_ON_ERROR(esp_console_start_repl(repl), TAG, "start_repl");
    ESP_LOGI(TAG, "DSP console ready - type 'help'");
    return ESP_OK;
}

