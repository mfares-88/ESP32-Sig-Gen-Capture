# CLAUDE-md-file-from-ard-sim.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository layout

Two independent codebases that communicate over USB serial at 115200 baud:

- `ardustim/` — Arduino firmware (C/C++). Target: ATmega328 (Nano/Uno) primarily; Mega2560 supported with different pin assignments. Built with PlatformIO or Arduino IDE.
- `UI/` — Cross-platform Electron app (Node.js) that talks to the firmware and ships a bundled `avrdude` for one-click flashing of `firmwares/nano.hex`.

`docs/`, `README.md`, `Wheel_Notes` (per-pattern frequency math), and `TODO` are reference material.

## Common commands

Firmware (from `ardustim/`):

```
pio run -e nano                       # build for Arduino Nano (default ATmega328)
pio run -e uno                        # build for Uno
pio run -e diecimilaatmega328         # build for older Diecimila
pio run -e nano -t upload             # build + flash
pio device monitor -b 115200          # serial console
```

The Arduino IDE alternative: open the `ardustim/` subfolder — no third-party library dependencies, unlike the upstream LibreEMS version which used SerialUI.

GUI (from `UI/`):

```
npm install                           # runs electron-rebuild -f as a postinstall
npm start                             # launch the Electron app for development
npm run package-win                   # build portable .exe via electron-builder
npm run package-mac                   # build .dmg (Intel)
npm run package-linux                 # build .AppImage
npm run package-arm                   # build .AppImage for armv7l
```

No automated test suite exists — `npm test` is a stub. CI (`.github/workflows/build.yml`) runs the packaging commands on Ubuntu/macOS/Windows and uploads the artifacts; the macOS arm64 job is intentionally disabled because of code-signing requirements.

## Architecture

### Firmware pulse-generation pipeline

The firmware is a hard-real-time signal generator built around AVR Timer1:

1. `wheel_defs.h` contains a giant table of `PROGMEM` byte arrays, one per trigger pattern. Each byte's lower bits drive ports for crank (PB0/pin 8), cam1 (PB1/pin 9), and cam2 (PB2/pin 10).
2. `ardustim.ino`'s `Wheels[]` array (in `setup`-adjacent globals) binds each pattern to a friendly name, edge count, `rpm_scaler`, and whether the cycle spans 360° or 720°. **Adding a new pattern requires editing both `wheel_defs.h` (the data) and the `Wheels[]` table (the metadata) — the order in `Wheels[]` is the wire-protocol wheel ID.**
3. `ISR(TIMER1_COMPA_vect)` walks `edge_states_ptr[edge_counter]` and writes the byte to `PORTB` XOR'd with `output_invert_mask`. Timer1's OCR1A defines the period between edges; changing OCR1A changes effective RPM.
4. `reset_new_OCR1A()` computes the OCR value as `8000000 / (rpm_scaler * rpm)` and picks one of five prescalers (1/8/64/256/1024) via `get_prescaler_bits()` so the result fits in a uint16. The prescaler swap is deferred to the next ISR via the `reset_prescaler` flag.
5. The main `loop()` handles serial commands and adjusts `currentStatus.base_rpm` according to `config.mode` (`FIXED_RPM`, `LINEAR_SWEPT_RPM`, `POT_RPM` — A0 ADC via free-running ADC ISR).
6. **Compression simulation**: below `config.compressionRPM`, `calculateCompressionModifier()` subtracts a sinusoidal modifier from base RPM to mimic cranking compression strokes. The waveform comes from `sin_100_180/120/90` PROGMEM tables in `globals.h`, selected by cylinder count. `compressionDynamic` scales amplitude linearly with RPM.

### Serial protocol (firmware ↔ GUI)

`comms.cpp::commandParser()` is a single-byte command switch. The protocol is **positional and binary** — no framing, no acks. Both sides assume the other is in lockstep. Key commands the GUI uses:

- `c` / `C` — receive / send the full `struct configTable` byte-for-byte (skipping the version byte on receive). **`globals.h::configTable` is the wire format**; the struct is `__attribute__((packed))` and `CONFIG_SIZE` in `UI/renderer.js` must match `sizeof(configTable)`. Changing any field reshapes the protocol on both sides.
- `L` — stream wheel names (one per line). `n` — number of wheels. `N` — current wheel index. `S<byte>` — set wheel.
- `P` — dump current wheel's edge array as CSV, followed by a second line with `wheel_degrees`. Used by `scope_generator.js` to render the pattern preview.
- `R` — current RPM. `r<lo_hi><hi_hi><int_hi>` — switch to sweep mode and set low/high/interval as three 16-bit words (big-endian via `word(hi, lo)`... read the code carefully when touching this).
- `s` — persist `config` to EEPROM via `storage.ino::saveConfig`.

`VERSION` (in `globals.h`) and `FW_VERSION` (in `UI/renderer.js`) must agree; the GUI uses this to detect outdated firmware and prompt re-flash.

### GUI process model

Standard Electron split:

- `main.js` — main process. Owns the `uploadFW` IPC handler which invokes the platform-specific `avrdude` binary under `bin/avrdude-<platform>/` against `firmwares/nano.hex`. Auto-detects old vs. new Nano bootloader by watching avrdude stderr for `resp=0x00`/`resp=0x01` and retrying at 57600 baud. **`contextIsolation: false` and `nodeIntegration: true`** are intentional so `renderer.js` can `require('serialport')` directly.
- `renderer.js` — handles serial I/O via `serialport`, parses USB VID/PID to label ports (Arduino Mega `2341`, Nano clone `1a86:7523`, Teensy `16c0`), polls the firmware for status, and pushes config writes.
- `gear_generator.js` / `scope_generator.js` — render the toothed-wheel visualization and oscilloscope-style edge preview from the `P`-command payload.
- `firmwares/nano.hex` is shipped pre-built and is what end-users flash; it is NOT regenerated automatically by CI from the `ardustim/` sources — keep it in sync manually when releasing.

## Things to know before editing

- **Wheel ordering is part of the API.** Reordering or inserting into `Wheels[]` invalidates any saved EEPROM `config.wheel` and any GUI assumptions about indices. Append, don't insert.
- **`configTable` is packed and version-tagged.** When you add a field, bump `VERSION` and `FW_VERSION`, update `CONFIG_SIZE` in `renderer.js`, and handle the migration in `storage.ino::loadConfig` (it currently rejects mismatched versions and falls back to defaults).
- **The Timer1 ISR runs at audio frequencies on big wheels.** A 720-edge pattern at 15k RPM is ~180 kHz — adding work inside `ISR(TIMER1_COMPA_vect)` will glitch the output. Keep it to the PORTB write, counter increment, and the prescaler-reset check.
- **Pin assignments differ by board.** Nano/Uno use pins 8/9/10; Mega uses 53/52/51. Code uses `__AVR_ATmega1280__`/`__AVR_ATmega2560__` to switch.
- **Sweep mode is implemented in `loop()`, not Timer2.** Timer2's ISR is set up but `TIMSK2 |= OCIE2A` is intentionally commented out — sweep timing comes from `micros()` comparisons in the main loop.
