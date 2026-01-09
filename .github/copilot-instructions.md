# Copilot / AI dev instructions for ESP32_Sig_Gen_Capture

Purpose: Give an AI code agent the exact, discoverable facts needed to be productive in this repo.

Quick start
- Build & upload (PlatformIO):

```bash
# build for the esp32-s3-n4r8 env
pio run -e esp32-s3-n4r8
# upload to device (uses upload settings from platformio.ini)
pio run -e esp32-s3-n4r8 -t upload
# open serial monitor (uses monitor_speed = 921600 from platformio.ini)
pio device monitor -e esp32-s3-n4r8
```

High-level architecture (big picture)
- `src/main.cpp`: orchestrates FreeRTOS tasks, CLI, and wiring between UI and signal capture/generator.
- `lib/ckp_gen/`: TimerCkpGenerator implementation and `SignalConfig` API — the slot-based CKP generator runs in a hardware timer ISR.
- `lib/ui_lvgl/`: LVGL-based UI wrapper. Use `ui_init(on_rpm, on_pattern)` and call `ui_task_handler()` frequently.
- Communication: main uses FreeRTOS `QueueHandle_t` (`gCtrlQ`) messages to send RPM/pattern/custom/start/stop to the manager and generator.

Critical developer workflows & signals
- Serial settings: `platformio.ini` sets `monitor_speed = 921600`, `monitor_filters = send_on_enter, time, esp32_exception_decoder`. Use the same when attaching a monitor.
- Upload speed / env: `upload_speed = 921600` and envs are defined in `platformio.ini` (`esp32-s3-n4r8`, `esp32-wroom32d`). Prefer `-e esp32-s3-n4r8` for local dev with PSRAM.
- Pins configured in `src/main.cpp`: `PIN_CKP_OUT` (CKP output) and `PIN_CAPTURE_IN` (capture input). Change there or call `TimerCkpGenerator::begin(pin)`.

Project-specific conventions & patterns
- Generator logic lives in `lib/ckp_gen` (slot-based, ISR-driven). If changing timing/state, update `TimerCkpGenerator` and honor the `portENTER_CRITICAL` / `portEXIT_CRITICAL` usage for shared volatile state.
- Use `SignalConfig` for all generator changes; the `apply()` method computes derived values (slots, slot_us) and atomically swaps config.
- UI code lives in `lib/ui_lvgl` and exposes two callbacks: `ui_on_rpm_cb` and `ui_on_pattern_cb`. The app wires these to enqueue control messages.
- Capture logic uses `EdgePulseCapture` (in `src/main.cpp`) and a FreeRTOS queue to deliver `CaptureReport` structs. Prefer queue-based comms over global polling.

Integration points & external deps (from `platformio.ini`)
- LVGL (`lvgl@^9.2.2`) and `Arduino_GFX_Library` for the display.
- `TAMC_GT911` touch driver for the touch controller.
- See `platformio.ini` for exact pinned deps.

Examples (patterns & CLI)
- Predefined patterns in `src/main.cpp`: e.g. "60-2", "36-1", "36-2", "36-1-1". CLI commands supported by the running firmware:

```
rpm <value>
pattern <index>
custom rpm=<val> teeth=<val> pmiss=<val> nmiss=<val> pos=<s|e> lvl=<h|l>
start
stop
```

When editing code
- Preserve separation: hardware/timing code in `lib/ckp_gen`, display/UI in `lib/ui_lvgl`, orchestration and RTOS messaging in `src/main.cpp`.
- Keep ISR time short and avoid heap allocations inside ISRs — existing code uses queues and critical sections correctly; follow that pattern.

Useful files to inspect first
- `platformio.ini` — build/envs, monitor speed, deps
- `src/main.cpp` — CLI, FreeRTOS manager, pin defs, capture wiring
- `lib/ckp_gen/CkpGenerator.h` / `CkpGenerator.cpp` — generator API and ISR logic
- `lib/ui_lvgl/ui_lvgl.h` / `ui_lvgl.cpp` — display and UI callbacks

If something's unclear
- Ask for runtime logs (`pio device monitor -e esp32-s3-n4r8`) and note where the serial prints originate (look for `DBG_PRINTLN` / `Serial.println`).

End of file
