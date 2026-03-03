# Codex Review 1

## Scope
This review focuses on the current ESP32 signal generator UI (LVGL), control/ISR logic, and error handling based on `src/main.cpp`, `lib/ui_lvgl/ui_lvgl.cpp`, and `lib/ckp_gen/CkpGenerator.cpp`.

## 1) Front End (LVGL UI)

### Issues
- Pattern list in the UI omits the 12-1 (index 4) pattern even though it is supported in the backend. This makes the UI and CLI behavior inconsistent. See `lib/ui_lvgl/ui_lvgl.cpp` and `src/main.cpp`.
- UI state is not synchronized with generator state: the UI has no indicator for running/stopped or for the current pattern selected if a CLI command changes it. See `lib/ui_lvgl/ui_lvgl.cpp` and `src/main.cpp`.
- RPM control accepts values 100..6000 by range but the UI does not show validation feedback when the backend clamps values. See `lib/ui_lvgl/ui_lvgl.cpp` and `src/main.cpp`.
- UI initialization fails hard with `while(true)` on display or draw buffer allocation failure, with no user-facing error on serial/GUI except one print, and no fallback. See `lib/ui_lvgl/ui_lvgl.cpp`.
- Touch input read uses the first point without basic filtering or debounce, which can cause noisy updates on real hardware. See `lib/ui_lvgl/ui_lvgl.cpp`.

### Recommendations
- Add the missing pattern entry to the dropdown so UI and CLI match, or remove pattern 4 from backend if not intended. Update `lib/ui_lvgl/ui_lvgl.cpp` and validate against `src/main.cpp`.
- Add UI-to-backend state synchronization: expose a function to set UI pattern selection and running state when CLI changes arrive. Create a small UI API in `lib/ui_lvgl/ui_lvgl.h` and call it from `src/main.cpp`.
- Provide explicit UI feedback when RPM is clamped (e.g., flash label or show min/max). Do this inside `on_arc_changed` and/or after backend confirmation in `src/main.cpp`.
- Replace infinite loops on init failure with a recoverable error screen and a safe stop. At minimum, print a clear error and allow serial command control to continue. See `lib/ui_lvgl/ui_lvgl.cpp`.
- Add lightweight touch filtering (ignore repeated identical points within a small time window) to prevent rapid queue spam and improve UX in `lib/ui_lvgl/ui_lvgl.cpp`.

## 2) Back End (Control logic, ISR, CLI)

### Issues
- `SignalConfig` validation is incomplete: only RPM is constrained; `pMiss`, `nMiss`, and `nTeeth` can be set to invalid values via CLI/custom, and invalid combinations can produce divide or gap sizing errors. See `src/main.cpp` and `lib/ckp_gen/CkpGenerator.cpp`.
- Gap math can become inconsistent (e.g., `gapSlots > slotsPerPeriod`), which can create undefined waveform behavior. See `lib/ckp_gen/CkpGenerator.cpp`.
- CLI parsing uses `String` operations extensively in the main loop. On embedded targets this can fragment the heap over time. See `src/main.cpp`.
- ISR capture uses `digitalRead()` in ISR, which is slow and can impact edge timing. See `src/main.cpp` (EdgePulseCapture).
- `xQueueSend` and `xQueueSendFromISR` return values are ignored; queue overflow is silently dropped. See `src/main.cpp`.

### Recommendations
- Add a centralized `validateConfig()` that enforces `nTeeth > nMiss`, `pMiss > 0`, `pMiss` divides slots per rev cleanly, and `gapSlots < slotsPerPeriod`. Reject or clamp invalid values. Use in `managerTask()` and in the CLI custom handler in `src/main.cpp` before calling `genTX.apply()`.
- In `TimerCkpGenerator::apply`, return a status (bool) and make callers report failures. This improves visibility for invalid settings. Update `lib/ckp_gen/CkpGenerator.h`, `lib/ckp_gen/CkpGenerator.cpp`, and `src/main.cpp`.
- Replace `String` parsing with a fixed-size buffer and `strtok`/`strtol` to reduce heap churn. See `src/main.cpp`.
- Use direct GPIO register reads in `EdgePulseCapture::onEdgeISR()` or move capture to RMT/PCNT to improve timing precision. See `src/main.cpp`.
- Check queue send return values and track overflow counters for diagnostics. Log to serial when the queue is full. See `src/main.cpp`.

## 3) Error Handling

### Issues
- Several critical initialization calls do not check success (queue/task creation, timer setup). Failures could leave the system in a partially initialized state. See `src/main.cpp` and `lib/ckp_gen/CkpGenerator.cpp`.
- Fatal errors loop forever without a clean safe mode or fallback, making recovery and diagnosis harder. See `lib/ui_lvgl/ui_lvgl.cpp`.
- The generator `apply()` silently returns on invalid inputs without notifying callers. See `lib/ckp_gen/CkpGenerator.cpp`.
- CLI prints success messages even when backend operations could fail (e.g., queue full). See `src/main.cpp`.

### Recommendations
- Add explicit checks for `xQueueCreate`, `xTaskCreatePinnedToCore`, `timerBegin`, and `timerAttachInterrupt` with error handling paths. Log errors and stop generator cleanly if init fails. Update `src/main.cpp` and `lib/ckp_gen/CkpGenerator.cpp`.
- Replace infinite loops with a safe fallback mode that keeps the serial CLI alive and clearly reports the error. Update `lib/ui_lvgl/ui_lvgl.cpp`.
- Change `TimerCkpGenerator::apply` to return `bool` and bubble failure up to the UI/CLI so the user knows why a command failed. Update `lib/ckp_gen/CkpGenerator.h`, `lib/ckp_gen/CkpGenerator.cpp`, and `src/main.cpp`.
- Add error acknowledgements in the CLI to avoid false positives. For example, print "queue full" or "invalid config". Update `src/main.cpp`.

## High-Impact Optimization Ideas (Cross-Cutting)
- Move capture to PCNT/RMT for accurate edge timing and lower ISR overhead. This will reduce jitter and CPU load compared to `digitalRead()` in the ISR. See `src/main.cpp`.
- Consider a configuration/state struct with atomic update (or message queue only) to avoid divergence between UI, CLI, and ISR state. This would reduce subtle state mismatches and simplify debugging.

