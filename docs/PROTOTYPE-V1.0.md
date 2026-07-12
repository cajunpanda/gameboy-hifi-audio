# Prototype v1.0: Bring-Up Findings and Troubleshooting

Working notes from bringing up the first fabricated main board (prototype v1.0).
This is kept separate from the main hardware and firmware guides so those stay
clean of prototype-specific troubleshooting. Content here is bring-up state and
defects found on this revision; some of it is resolved in later board revisions.

First-board bring-up date: 2026-07-03. Module BT MAC on the first unit:
`c0:cd:d6:35:e3:46`. Firmware v0.1.0 on ESP-IDF 6.0.1.

## Board status summary

| Subsystem | Result |
|-----------|--------|
| Power (U3 TPS61021A boost, 3.3 V rail) | OK when fed from the battery rail |
| ESP32 boot, 8 MB flash, partition table | OK |
| ES8388 codec over I2C (config + read-back verify) | OK |
| I2S master clocking (44.1 kHz, MCLK/APLL on GPIO0) | OK |
| LittleFS clip store, DSP, SFX cue player | OK |
| Bluetooth controller + A2DP + BLE config server | OK |
| Audio path end to end (GBA tap -> codec -> DSP -> DAC -> amp -> speaker) | OK, real game audio heard |
| Speaker amp U4 (PAM8302A) | DEFECT: DC-offset / self-heating, see below |
| Antenna connector generation (U1 -02U) | DEFECT found on first production run, see below |

## Bench rig setup

### Serial over the Tag-Connect / FT232H cable

The bench PC has a udev rule (`/etc/udev/rules.d/50-programmer_usb.rules`) that
force-unbinds `ftdi_sio` from any `0403:6014` (FT232H) device and re-triggers
itself with `udevadm trigger` plus `modprobe -r ftdi_sio`, so libftdi/JTAG tools
can claim the chip. The Tag-Connect TC2030-NL-FTDI-C232HD serial cable is an
FT232H with that same VID:PID, so it enumerates on USB but never gets a
`/dev/ttyUSB0`, and a manual `bind` returns EBUSY because the rule keeps ripping
it off.

Fix: comment out the three `6014` lines in that rules file (leave the `6010`
FT2232H JTAG lines intact), `sudo udevadm control --reload-rules`, then replug
the cable. The port then comes up stable as
`/dev/serial/by-id/usb-Tag-Connect_TC2030-NL-DDHSP-0-DTR_FTBELUPJ-if00-port0`.
The cable carries DTR, so esptool auto-reset into the bootloader works.

### Powering the board on the bench

The board runs one 3.3 V rail from the TPS61021A boost converter (U3), fed by
the GBA switched battery rail. There is no other regulator, and the Tag-Connect
cable is not a power source.

Do not power the board through the C232HD cable's VCC pin. That pin is
FTDI-limited (about 250 mA, no bulk cap, slow transient), so the rail collapses
below the ESP32 brownout threshold the instant the Bluetooth radio does its full
RF calibration: `E BOD: Brownout detector was triggered` -> `rst:0x3` -> an
infinite reboot loop that dies at the same phy_init spot every time.

Feed the battery rail instead, from a bench supply at about 3.0 V with the
current limit at 1 A (clip onto the battery-rail pads if possible; feeding
through the GBA battery contacts is marginal). The board then runs the full
stack cleanly.

Bench power-up order matters: connect the serial cable first, then apply rail
power. This always produces a few brownout reboots at the start while the supply
settles; those initial `BOD` events are expected and benign, not a power-margin
warning.

## Open defect: U4 speaker amp (PAM8302A) DC offset and self-heating

Symptom on the first board: with the speaker amp enabled, the board draws about
300 mA over its base current, U4 gets hot to the touch, and the speaker output
is very noisy. The draw is fixed regardless of digital volume (`vol 0` vs
`vol 40`) and regardless of the codec output level (`out2 0x00`), and it drops
back to idle only when the amp is shut down (app_sm deasserts `PIN_PAM_SD`, done
on the bench with `hp plug`). Real game audio is still audible on top of the
noise, so the signal path works.

Root cause (confirmed): **U4 pin 4 (IN-) is tied directly to DC ground instead
of being AC-coupled.** The PAM8302A requires its inputs to be capacitively
coupled ("an input capacitor CI is required to bias input signals to a proper DC
level"). On this board IN+ (pin 3) is correctly coupled through C19 (1 uF), but
IN- (pin 4) sits on the GND net with no series cap. Hard-grounding IN- defeats
the internal input bias on that leg, so the amplifier sees a large DC
differential and drives the bridge-tied output to a DC offset.

Confirming measurement: **2.7 V DC between SPK+ and SPK-** on a 3.3 V rail (the
output is essentially railed). 2.7 V across an 8 ohm speaker is about 340 mA of
DC, which accounts for the ~300 mA draw and the heat. A healthy BTL Class-D
idles near 0 V differential.

Contributing factors on this revision:

- Gain: the earlier note here (that the amp ran at its ~23.5 dB max for lack of
  an external input resistor) was wrong. R7 and R8 (15k each) sum LOUT2/ROUT2
  into PAM_SUM and sit in series with IN+, so they already act as the external
  RIN in A = 20*log(150k/(10k+RIN)). Effective gain is ~12.6 dB per channel
  (~18.7 dB when both codec outputs carry the same signal), well under the
  23.5 dB IC ceiling. Gain is already set by R7/R8; raise them to lower it. The
  gain amplifies the residual noise floor, but the amp is not near max gain.
- Output EMI: the earlier note here (filterless output onto "long flex speaker
  leads") was wrong for this board. SPK+/SPK- (J5/J6) are 4-6 mm from U4 on the
  PCB, so the filterless output is in its intended short-lead use case and the
  VDD bulk (C20/C21) covers it. An output ferrite/Zobel is only warranted if the
  off-board speaker lead itself is long (datasheet: >~20 cm).

VDD bypass (C20 10 uF and C21 100 nF on +3V3) is placed at the pin and is not
implicated; the fault is the input DC path, not decoupling.

Fixes for the next revision:

1. AC-couple IN-: add a coupling cap from IN- to GND matching C19 (about 1 uF).
   This is the actual fix. No matching resistor on IN- is needed -- the input is
   single-ended (signal on IN+, IN- is the AC-grounded reference leg) and both
   caps block DC, so the internal bias is already balanced.
2. Gain, if adjustment is wanted, is set by R7/R8 (15k) -- increase them (keep
   the pair matched to hold L/R balance); no new part. The -10.5 dB codec cut
   applied in firmware is the tell that system gain is ~10 dB hotter than used,
   so moving that attenuation into R7/R8 (~15k -> ~47k) reclaims codec SNR.
3. Output EMI filter is not needed (speaker pads are at the amp, see above).
   Optional DNP hedge: leave bead+cap footprints per output in case on-board
   2.4 GHz (now a built-in PCB antenna) shows desense in EMC testing.

Fix 1 validated on the first board by bodge (2026-07-04): IN- (pin 4) lifted off
GND and a 1 uF ceramic added from IN- to GND, value-matched to C19 on IN+ (both
form the input high-pass with the amp's internal ~10k, corner ~16 Hz at 1 uF).
SPK+/SPK- DC offset went from 2.7 V to 0 V, U4 runs cool, and the brownout/cook
cycle is gone. Carry the IN- coupling cap into Rev B.

Firmware guard for continued bring-up: boot with the speaker amp disabled by
default (leave `PIN_PAM_SD` deasserted, enable only by an explicit console
command). SD already has a 100k pulldown, so leaving it undriven keeps the amp
off through resets and stops the reboot-then-cook cycle while the rest of the
board is validated.

## Mode A / Mode B (battery saver) transitions

Mode B is the full DSP path; Mode A is analog bypass with the digital path
stopped and the CPU in a duty-cycled light sleep (~200 ms wake to poll the VOL
wheel), ~40 mA at the board vs ~80 mA in Mode B at matched loudness (confirmed on
the first board, 2026-07-04). Bidirectional transitions tested clean: B -> A via
the `mode a` console command, A -> B via the R-button hold, which rebuilds
I2S/MCLK, resumes the pipeline, and restores the codec to DSP.

Two findings:

- Mode A speaker level overdriven and louder than Mode B (fixed). The Mode A
  analog-volume ceilings (`ES8388_LINE_VOL_MAX` / `ES8388_HP_VOL_MAX` in
  es8388.c) were stale relative to the tuned Mode B driver levels: line out was
  0x21 (+4.5 dB) and HP 0x0e (-24 dB), while Mode B uses 0x17 (-10.5 dB) and 0x14
  (-15 dB). At full wheel Mode A hit those ceilings, so switching B -> A jumped
  the loudness and drove the speaker into the amp's clipping. Fixed by making the
  ceilings the single source of truth for the tuned levels, shared by es8388_init()
  and es8388_set_output_volume(), so the two modes stay in lockstep.

- The console cannot exit Mode A over serial (by design, not a bug). In Mode A
  the ESP32 is in light sleep ~98% of the time; the UART is not a wake source and
  the console task never gets scheduled, so a serial `mode b` does not land. The
  real exit is the physical R-button hold (an ext1 wake that mode_a_run() handles;
  the loop re-checks the mode preference each wake so a console/BLE change only
  lands "if it happens to fall in a wake window"). A cabled `reset` also returns
  to Mode B since the mode preference is not persisted unless saved.

## Power profile (bench, 3.2 V rail)

Rail current per operating mode, measured 2026-07-09 on the bench supply (FNIRSI
DPS-150 at 3.2 V, i.e. 2xAA) with the GBA running the Audio Test ROM's 1 kHz
tone. All figures are **total rail current = GBA console + mod board**; the GBA
alone is isolated by holding the ESP32 in reset (EN low). Mode A/B volume is the
VOL wheel (the console sleeps in Mode A), speaker vs headphone by the jack.

| Operating point | I @ 3.2 V | P |
|---|---|---|
| GBA baseline (ESP32 held in reset) | 0.096 A | 0.31 W |
| Mode B (DSP) speaker — 0 / 50 / 100 % | 0.134 / 0.134 / 0.157 A | 0.43 / 0.43 / 0.50 W |
| Mode B headphone (any volume) | 0.140 A | 0.45 W |
| Mode A (bypass, light-sleep) speaker — 0 / 50 / 100 % | 0.126 / 0.126 / 0.148 A | 0.40 / 0.40 / 0.47 W |
| Mode A headphone — 50 % | 0.122 A | 0.39 W |
| Bluetooth A2DP streaming to a sink | 0.228 A | 0.73 W |

Findings:

- **Bluetooth streaming is the heaviest mode by far** — 0.228 A, ~+130 mA over
  the GBA baseline. SBC encode plus BR/EDR radio TX (at -3 dBm) dominates, well
  above any local-playback state. Primary target for battery-life work when BT is
  in use.
- **Mode A runs measurably below Mode B** at matched loudness — the analog bypass
  plus duty-cycled light sleep avoids the always-on DSP overhead (Mode A headphone
  at 0.122 A is the lowest active state). Confirms the battery-saver intent.
- **Speaker output only draws extra current above ~50 % wheel** (0 -> 50 % is flat
  in both modes; 1 kHz into the small speaker). Headphone draw is
  volume-independent — the cost is simply the HP amp being on.

Measurement note: the initial Bluetooth reading came out spuriously low
(0.117 A) — the DPS-150's current sense had momentarily misread (caught when it
diverged from the front panel); a full unit power-cycle (not just a USB replug)
restored it and BT re-measured at 0.228 A. The other rows are consistent with
prior bench observations.

### Update 2026-07-12: with a HiSpeedido v5 IPS screen

Re-measured on a unit fitted with a **HiSpeedido v5 IPS** screen mod (same DPS-150
at 3.2 V), this time with the GBA running live game audio rather than the 1 kHz
test tone. Total rail current = GBA console + IPS screen + mod board; the mod was
isolated for the baseline by holding the ESP32 in reset (EN low). **The IPS
backlight was at full brightness (level 15 of 15)** for every figure in the table
below.

| Operating point | I @ 3.2 V | P | Δ vs IPS baseline |
|---|---|---|---|
| GBA + IPS baseline (ESP32 held in reset) | 0.263 A | 0.84 W | — |
| Mode A (bypass, light-sleep) speaker 100 % | 0.288 A | 0.92 W | +25 mA |
| Mode B (DSP) speaker 100 % | 0.311 A | 0.99 W | +47 mA |
| Bluetooth A2DP streaming to a sink | 0.404 A | 1.29 W | +141 mA |

- **The IPS screen is the single biggest load.** It adds ~167 mA over the bare
  console (0.263 A here vs the 0.096 A bare-GBA baseline above) — more than any
  mod mode. On an IPS-modded unit the backlight dominates the power budget.
- Mode ordering holds: **Mode A < Mode B < Bluetooth**. The mod's own overhead is
  ~+25 mA (Mode A), ~+47 mA (Mode B: ~38 mA DSP + light speaker drive), and
  ~+141 mA (BT: ~38 mA DSP + ~100 mA SBC encode + BR/EDR radio at -3 dBm).
- This run used **live game audio** (vs the 1 kHz tone in the table above), so the
  speaker-drive component varies with content — per-mode figures carry ±10-30 mA
  against the tone table, while the digital-core deltas match.
- **Backlight brightness is a large, independent lever.** Dropping the IPS from full
  (level 15) to half (level 8) takes the baseline from 0.263 A to **0.216 A** — a
  ~47 mA saving, as much as the entire Mode B DSP overhead. Every GBA + IPS figure
  scales down ~47 mA at half brightness, so runtime improves accordingly (baseline
  7.6 h -> ~9.3 h; Bluetooth streaming 4.9 h -> ~5.6 h). Turning the backlight down
  is the single most effective battery-life lever on an IPS unit.

### Battery life estimate (2x AA alkaline)

The two AA cells power the GBA in **series** (~3.0 V fresh), so both carry the same
current and the pack's usable capacity equals one cell's (series adds voltage, not
capacity). Runtime is then just:

    runtime (h) = usable capacity (mAh) / rail current (mA)

Assuming **~2000 mAh** usable for AA alkaline at these drains down to the GBA's
brownout (~2.0 V pack). This is deliberately conservative — light loads realistically
reach ~2400 mAh; scale linearly for other cells (e.g. 2500 mAh multiplies every
figure by 1.25).

| Operating point | Bare GBA | GBA + IPS |
|---|---|---|
| Baseline (mod idle / held in reset) | 96 mA -> **20.8 h** | 263 mA -> **7.6 h** |
| Mode A speaker, 100 % volume | 148 mA -> **13.5 h** | 288 mA -> **6.9 h** |
| Mode B speaker, 100 % volume | 157 mA -> **12.7 h** | 311 mA -> **6.4 h** |
| Bluetooth streaming to a sink | 228 mA -> **8.8 h** | 404 mA -> **4.9 h** |

Bare-GBA column uses the 2026-07-09 tone figures; GBA + IPS column uses the
2026-07-12 live-audio figures. The headline: **the IPS screen roughly halves
runtime in every mode** — it, not the audio mod, is the dominant battery cost. With
the mod streaming Bluetooth on an IPS unit, expect ~5 hours from a fresh pair of AAs.

These are **charge-based** (capacity / fresh-battery current) and so run slightly
optimistic: the GBA + IPS load sits on the raw battery rail (~constant current, so
`Q/I` holds), but the mod board runs off the U3 TPS61021A **boost** — a constant-power
load whose input current climbs ~30 % as the cells sag from ~3.2 V to ~2.0 V. The
real hit is ~10-17 % on the Bluetooth rows (largest boosted fraction), less in Mode
A/B, and negligible at baseline — and it partly cancels against the conservative
2000 mAh assumption (realistic alkaline delivers ~2400 mAh at these drains). An exact
figure needs an energy (Wh) model with the boost efficiency curve; for planning these
are fine.

## Low-battery boot brownout at BT radio init (resolved in firmware, 2026-07-11)

At a low battery voltage (bench: DPS-150 at 2.4 V into the GBA battery
terminals) the board brownout-looped forever at boot, dying at the same log line
every cycle: `phy_init: phy_version ...` -> `E BOD: Brownout detector was
triggered`. That line is inside `esp_bt_controller_enable()`: the RF PHY's
boot-time partial calibration fires TX bursts through the radio — the largest,
fastest current step of the whole boot — and it landed while the startup chime
was still playing at full wheel volume.

**Why it only happens at low VIN.** The TPS61021A's 1.5 A @ 3.3 V (VIN > 1.8 V)
rating is steady-state; a µs-scale load step is carried by the output caps until
the control loop catches up, and the effective rail capacitance is small
(C2 = 0603 6.3 V 22 µF derates to ~9 µF at 3.3 V bias; ~20 µF effective
rail-wide). The gating fact: during a transient a boost's output can't fall
below roughly VIN minus a body-diode drop. At VIN 3.2 V that floor (~2.8 V) is
*above* the ESP32's brownout threshold (level 0, ~2.43 V — already the lowest
setting), so BOD is physically unreachable; at VIN 2.4 V the floor (~2.0 V) is
below it, and a deep-enough transient trips it. Raising the bench current limit
1 A -> 3 A changed nothing (it is not supply foldback), and the supply reads a
steady 2.400 V CV throughout — a 20 Hz sample never sees the sub-ms dip.

Resolved in firmware (RF-cal skip, load serialization, TX cap, brownout loop
breaker — see FIRMWARE.md "Init order") and verified cold-booting cleanly at
2.4 / 2.2 / 2.0 V; 2.0 V is below any usable 2xAA level, the GBA console itself
gives out first. Bench notes: phantom power must be killed for a true cold boot
(`tools/bringup.py --kill-phantom`), and the onboard `batt` reading sits ~56 mV
under the DPS setpoint (lead + FPC drop), e.g. 2344 mV at a 2.4 V setpoint.

### Suggested hardware changes (next board revision)

The firmware fix removes the trigger, but the underlying transient margin is
thin. For the next rev, in rough order of value:

1. **More *effective* bulk on VBOOST / +3V3.** C2 (22 µF, 0603, 6.3 V X5R,
   CL10A226MQ8NRNC) is ~9 µF at 3.3 V bias. Swap to a 10 V-rated 0805 22 µF
   (halves the derating) and/or add a dedicated bulk cap at U3 VOUT — a 6.3 V
   100–220 µF polymer (or two more 22 µF ceramics) targeting >= 50 µF effective.
   Every µF directly extends how long the rail rides through a load step.
2. **Bulk at the PAM8302A supply pin.** The amp shares +3V3 with the ESP32;
   full-scale speaker peaks are hundreds of mA and coincide with whatever else
   the rail is doing. A local 22–47 µF (10 V rated) at U4 VDD decouples speaker
   program peaks from the ESP32's BOD sense point.
3. **Verify L1 saturation headroom.** L1 (0.47 µH, WIP252012P-R47ML, 2520) must
   not saturate below the TPS61021A's ~4.6 A peak switch limit; if its Isat is
   below that, the converter loses its rated transient capability exactly when
   it's needed. If marginal, move to a 0.47 µH part with Isat >= 4.5 A.
4. **More input bulk on VBAT at the board.** VBAT arrives over two FPC pins plus
   the battery-terminal wiring; the input transient (boost input current exceeds
   1.5x output current at 2.4 V in) sees all of that resistance. C1 is a single
   derated 22 µF; a second 22 µF (or one 47 µF, 10 V) at U3 VIN stiffens it.

## Boot latency and the startup chime

The mod boots on the GBA's switched rail, so the Game Boy power-on chime is the
first thing the user hears every power-cycle. Two problems on the first board, both
addressed 2026-07-05:

- **The mod stomped the chime with its own connect/pairing cue.** On boot the state
  machine reconnects a bonded sink (or enters pairing) and played the CONNECT /
  PAIRING synth cue right over the chime. Fixed with a one-shot `s_boot_arrival`
  latch in app_sm.c that suppresses only the first arrival cue after power-on; later
  reconnects (mid-session drop) still cue.

- **The chime's head was clipped by boot latency.** Nothing reaches the speaker
  until the codec + pipeline + amp are up. Originally the amp was unmuted inside
  `app_sm_start()` at ~2.8 s, behind the ~1.3 s Bluetooth init, so the first ~2 s of
  the chime was lost. Reworked the boot order (see FIRMWARE.md "Init order") to get
  audio out ASAP: unmute the amp right after `es8388_init()` (HP-aware), drop the
  codec read-back verify (a breadboard-era artifact; the PCB's series-R + short MCLK
  routing keep I2C writes clean), which together bring first audio to ~0.9 s.

That still clips the head, so the mod now plays its **own** copy of the chime: a
GSFX clip (`firmware/data/startup.gsfx`, a recording of the AGB chime) triggered at
boot with the live passthrough muted for the clip's duration (`dsp_begin_intro()`),
so there's no doubling with the truncated live chime. The clip follows the volume
wheel and routes to the live output (speaker vs headphones). It's gated so a future
setting can disable it and let the GBA's own chime play through untouched. The clip
lives in the LittleFS `storage` partition — reflash it with `serial_proxy.py flash
--fs`, not a plain `flash`.

Related fixes made in the same pass:

- **Reboot volume blare.** The VOL wheel was only read by app_sm's poll, now behind
  the BT hold-off, so early audio played at the stored default. `app_sm_prime_volume()`
  now reads the wheel and seeds the DSP volume before the first audio block.
- **Bluetooth start deferred** to `CONFIG_GBHIFI_BT_START_DELAY_MS` (default 3.5 s)
  after power-on, so the radio's inrush current and RF noise land after the chime
  rather than during it.

## Audio noise floor and reduction

The GBA's pre-volume audio tap is inherently noisy and the mod captures it
faithfully. Characterised with the `wavedump` console capture + tools/plot_wavedump.py
(radio off, GBA silent) on the first board:

- Dominant tone ~21.4 kHz: the GBA Direct-Sound PWM carrier (65.536 kHz) aliasing
  into band when the codec samples at 44.1 kHz (65536 - 44100 = 21436 Hz). The
  reconstruction low-pass (R3/C13, single-pole ~7.2 kHz) only knocks the carrier
  down ~20 dB. Faintly audible as a high whine.
- Low hum ~172 Hz plus a broadband floor near -100 dBFS. RMS ~-61 dBFS.
- The noise is on the tap, not the codec supply/reference (which are well filtered
  by FB1 + generous bypass): grounding RIN/LIN kills it, and it is attenuated by
  the digital volume wheel, so it enters on the ADC capture side. Partly the GBA's
  own audio, partly ground-difference coupling on the shared flex, which carries
  the boost/amp/BT switching return currents alongside the audio tap.

Reduction runs in the DSP (Mode B) on the captured signal before the voicing EQ,
tunable per GBA via the `nr` console command and persisted in NVS:

- HPF / LPF / notch biquads (hum / hiss / a discrete tone).
- A downward expander (noise gate): below the threshold the signal is attenuated
  so the floor fades out in quiet passages.
- Amp-mute on sustained silence: app_sm drops the PAM8302A (PIN_PAM_SD) once the
  gate has held silent, killing the Class-D idle hiss the DSP cannot reach, and
  releases it the instant audio returns.

Baked defaults (first-board tuning): HPF 100 Hz, gate -42 dBFS / 30 dB, rest off.
Result: dead-silent headphones and speaker on silence, clean on audio return.

Source-isolation bench test (2026-07-05): confirmed the residual speaker hiss is
capture-side, not the amp. Procedure -- `mode b`, `hp unplug`, `nr gate off`
(this also defeats the silence amp-mute, since the deep-silence detector in dsp.c
only runs inside `if (s_gate_on)`, so U4 stays enabled and the floor is audible).
Then:
- `out2 0x00` (mute the ES8388 speaker driver) -> hiss gone. It is in the codec
  output path, not U4 idle/switching or supply/ground coupling.
- Volume wheel (digital `vol`, applied pre-DAC in the DSP) attenuates the hiss ->
  it rides in the digital program signal, i.e. the ADC capture-side floor above,
  not the codec's post-DAC analog output-stage noise.

Consequence: raising R7/R8 (the U4 input/gain resistors, see the PAM8302A section)
does **not** help this hiss. The noise is already digitised, so scaling the DAC up
and the amp gain down leaves its SNR unchanged. R7/R8 only shave amp-input and
codec analog-output noise, neither of which is the dominant term here. The fix is
the DSP `nr` chain now (gate/HPF/notch), and the capture-path rework below in Rev B.
(Restore after the test: `out2 0x17` = -10.5 dB, or reboot -- the poke is not saved.)

Rev B items:

- Steeper anti-alias before the ADC (a 2-pole reconstruction LP, or a lower corner)
  so the 65.5 kHz PWM carrier cannot fold to 21.4 kHz. This is the real fix for the
  whine; a digital notch that close to Nyquist is weak.
- A differential / ground-referenced audio tap into the ES8388 (RIN1/LIN1 are NC)
  to reject the shared-flex ground-difference noise; keep the tap traces away from
  the switching returns on the flex.

### Bench gotcha: serial cable reboot loop

A bus-powered FTDI serial cable whose VCC (3.3 V) output is tied to the board's
+3V3 (the Tag-Connect VCC, J1 pin 1) gets back-driven by the board's switching and
amp transients, browns out, and re-enumerates on USB. The serial proxy then reopens
the port, and opening it asserts DTR -> EN -> a chip reset; the reset sags +3V3,
re-enumerates the FTDI again, and the loop self-sustains (POWERON_RESET every few
seconds). It only happens with the cable attached and the board active, which makes
it look mechanical -- it is not.

Fixes: use a serial cable that does not feed VCC (leave only TX/RX/GND/DTR), and/or
the serial_proxy.py open-without-DTR-glitch change. Rev B: do not hard-tie the
Tag-Connect VCC to +3V3.

## Antenna: U1 uses MHF III, not U.FL (found on the first production run)

The `-02U` module has an **external antenna connector**, not an on-board PCB
antenna. Per the Espressif datasheet it is a **third-generation I-PEX MHF III**
connector, also sold as **Hirose W.FL** or **Amphenol AMMC** — the three names
are the same interface. Order an antenna terminated in one of those.

Do **not** buy a U.FL / IPEX MHF1 antenna (the common default): it is physically
larger and will not seat on the MHF III connector. MHF4 is too small.

Native MHF III antennas are uncommon and mostly special-order, and the one FPC
type that is stocked (Kyocera `9000352F0`) is a *"for metal surfaces"* antenna
that detunes on a plastic shell. So the build uses a common **free-space
2.4 GHz FPC antenna** (designed for plastic enclosures) plus a small adapter to
bridge the connector generations:

- **`ANT1` — Taoglas `FXP70.07.0053A`**: adhesive flexible FPC, single-band
  2.4 GHz, ~53 mm lead, 1.1 dBi, terminated in **U.FL / IPEX MHF1**. Mount it
  against the plastic, with keep-out from metal and the PCB ground. Stocked,
  MOQ 1. For more range use `FXP73.07.0100A` (100 mm, 2.5 dBi).
- **`ADPT1` — MHF III (plug) → U.FL (jack) adapter**: a short (~50 mm) coax that
  presents an MHF III / W.FL **plug** to the module and a U.FL **jack** for the
  antenna. This is not a distributor part — order it from an RF pigtail vendor
  (Data Alliance, superbatrf, eteily) or AliExpress.

The adapter adds ~10 cm of total cable and two extra micro-connections, so keep
it as short as possible and coil the slack inside the shell. If you would rather
avoid the adapter, order the antenna itself terminated in MHF III (a special
order with MOQ/lead time) and drop `ADPT1`.

Do **not** substitute a U.FL / MHF1 antenna directly onto the module: the module
is MHF III and the two do not mate — that mismatch is what the adapter resolves.
The `FXP70`'s 1.1 dBi sits within the datasheet's ≤ 2.33 dBi certified envelope;
if you swap to a higher-gain antenna you may exceed the module's FCC/CE modular
grant. Both `ANT1` and `ADPT1` are cabled accessories (marked do-not-place) — not
assembled onto the board. Press each connector straight down to mate it; MHF III
is fragile and rated for only a handful of mating cycles.

### Bench bring-up

For integration testing you do not need the final in-shell antenna. The simplest
rig that mates the module directly is an external whip on a bulkhead pigtail:

- **W.FL → RP-SMA-female bulkhead cable** (0.81 mm coax), e.g. Data Alliance's
  W.FL-to-RP-SMA-female cable. The W.FL end is an MHF III **plug** that mates the
  module; the RP-SMA-female end takes any RP-SMA-male whip.
- **RP-SMA-male 2.4 GHz whip**, e.g. a 5 dBi articulating antenna.

Full chain: `module MHF III jack → W.FL plug —[coax]— RP-SMA-F bulkhead ←
RP-SMA-M whip`. The 5 dBi whip exceeds the module's ≤ 2.33 dBi certified
envelope, which is fine on the bench but must not ship in a certified product.
Do **not** try to re-terminate spare U.FL/MHF1 FPC antennas onto MHF III by hand:
that connector is a crimp-plus-center-pin-solder onto thin coax, not a solder
swap, and hand rework gives unreliable VSWR — use the pigtail instead.

### Production sourcing

MHF III antennas are uncommon off-the-shelf, but this needs a catalog antenna
with the MHF III connector option, not a bespoke antenna design. In order of
preference:

1. **2J Antennas** — flexible connector/cable-length configurator, friendlier
   MOQs; ships an FXP-class flex antenna with an MHF III plug and a chosen lead
   length. Removes `ADPT1`.
2. **Taoglas `FXP70`/`FXP73` with the MHF III connector code** — the same
   free-space antenna as `ANT1`, factory-terminated in MHF III (custom-line SKU,
   MOQ + lead time). Removes `ADPT1`.
3. **Kyocera `9000352F0-AE20L0050`** (50 mm MHF III) — MOQ 200, ~9-week lead; a
   normal buy at run sizes near 200, but it is the metal-surface type so back it
   with the PCB ground.

Budget an MOQ of a few hundred and an 8–12 week lead, and validate VSWR/range
with the antenna in its final position inside the actual GBA shell — the plastic,
battery, and PCB all detune it.
