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

- No external series gain resistor on the input, so the amp runs near its max
  gain (~23.5 dB from the internal 150k/10k). That amplifies the noise floor.
- Filterless output runs straight onto the long flex speaker leads with no
  output ferrite/Zobel network (an EMI concern, not the current draw).

VDD bypass (C20 10 uF and C21 100 nF on +3V3) is placed at the pin and is not
implicated; the fault is the input DC path, not decoupling.

Fixes for the next revision:

1. AC-couple IN-: add a coupling cap from IN- to GND matching C19 (about 1 uF).
   This is the actual fix.
2. Add a series input resistor (about 10 to 47 kohm) to set a sane gain.
3. Optional: add an output ferrite-bead plus cap EMI filter given the long flex
   speaker run.

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
  ceilings the single source of truth for the tuned levels, shared by
  es8388_init() and the verify table, so the two modes stay in lockstep.

- The console cannot exit Mode A over serial (by design, not a bug). In Mode A
  the ESP32 is in light sleep ~98% of the time; the UART is not a wake source and
  the console task never gets scheduled, so a serial `mode b` does not land. The
  real exit is the physical R-button hold (an ext1 wake that mode_a_run() handles;
  the loop re-checks the mode preference each wake so a console/BLE change only
  lands "if it happens to fall in a wake window"). A cabled `reset` also returns
  to Mode B since the mode preference is not persisted unless saved.

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
