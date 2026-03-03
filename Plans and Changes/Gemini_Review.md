# Codebase Review for ESP32 Crankshaft Signal Generator

## 1. Back End (Firmware & Logic)

### Strengths
- **Modular Architecture:** The project successfully separates concerns into `src/main.cpp` (Orchestration), `lib/ckp_gen` (Signal Generation), and `lib/ui_lvgl` (User Interface).
- **Thread Safety:** Correct usage of FreeRTOS queues (`gCtrlQ`) for communicating between the UI/CLI (running in tasks/loops) and the hardware management logic.
- **Critical Sections:** Use of `portENTER_CRITICAL` logic within the generator class to protect shared volatile state is commendable.

### Weaknesses & Issues
- **Dead Code Retention:** `src/main.cpp` contains significant blocks of commented-out code (lines 88-117 and 137-308) that were moved to the library. This clutter reduces readability and should be removed.
- **Lack of Modularization for Capture:** While the Generator logic was moved to a library, `EdgePulseCapture` (lines 314-368 in `main.cpp`) remains in the main file. It should ideally be moved to `lib/ckp_capture` or similar to maintain symmetry and cleanliness.
- **ISR Complexity & Performance:**
    - The `TimerCkpGenerator::onTimer` method contains conditional logic and modulo operations which can cause jitter.
    - **(New)** The Capture ISR (`EdgePulseCapture::onEdgeISR`) uses `digitalRead()`. This is significantly slower than direct register access and introduces unnecessary overhead in a time-critical interrupt handler.
- **Global State:** The reliance on global instances (`genTX`, `capRX`, `gCfg`) in `main.cpp` is typical for Arduino but makes unit testing isolated components harder.
- **Hardcoded Dependencies:** The generator class is tightly coupled to specific ESP32 hardware timer APIs (`timerBegin`, `timerAttachInterrupt`). Abstracting the timer interface would allow for easier porting or mocking in tests.
- **(New) Incomplete Config Validation:** While RPM is constrained, other `SignalConfig` parameters (`nTeeth`, `pMiss`, `nMiss`) lack validation. Invalid combinations (e.g., `gapSlots > slotsPerPeriod`) are not rejected, potentially causing undefined waveform behavior or divide-by-zero errors.
- **(New) Heap Fragmentation Risk:** The CLI parser relies heavily on the Arduino `String` class for command parsing. On embedded systems, this frequent allocation/deallocation pattern can lead to heap fragmentation over long uptimes.

## 2. Front End (LVGL UI & CLI)

### Strengths
- **Clean UI Implementation:** The LVGL code is well-structured, with a clear separation of style initialization, screen creation, and event handling.
- **Visual Feedback:** The UI provides immediate feedback (RPM labels updating with the Arc) which is good UX.

### Weaknesses & Issues
- **Pattern List Mismatch:** The CLI supports 5 patterns (indices 0-4, including "12-1"), but the UI dropdown (line 203 in `ui_lvgl.cpp`) only defines 4 options ("60-2\n36-1\n36-2\n36-1-1"). Users cannot select the 5th pattern via the GUI.
- **Manual String Parsing:** The CLI command parsing in `pollSerialForDemo` (especially the `custom` command) uses manual string manipulation which is fragile and error-prone.
- **Hardcoded UI Geometry:** Positioning of UI elements uses magic numbers (e.g., `lv_obj_align(..., 20, 24)`). Using layout containers (Flex/Grid) would make the UI more responsive to different screen sizes if the hardware changes.
- **Blocking Error States:** If the UI fails to initialize (e.g., `gfx->begin()` fails), the code enters an infinite blocking loop (line 249 in `ui_lvgl.cpp`), rendering the device useless without feedback.
- **(New) Lack of Bidirectional Sync:** The UI is not synchronized with the backend state. If a user changes the pattern or RPM via the CLI, the UI elements (Arc, Dropdown) do not update to reflect the new state, leading to confusion.
- **(New) No Touch Debouncing:** The touch input handler reads raw points without filtering or debouncing. This can lead to jittery input or accidental double-clicks on sensitive hardware.
- **(New) Missing Feedback for Clamping:** The UI allows selecting RPMs (via Arc) that might be outside the valid logical range (if logic differed), but more importantly, if the backend clamps a value (e.g., via a CLI command that exceeds limits), the UI gives no visual indication that the request was modified.

## 3. Error & Exception Handling

### Strengths
- **Parameter Constraints:** usage of `constrain()` for RPM values prevents invalid physics configurations.
- **Pointer Safety:** Good checks for null pointers (e.g., `if (s_inst)` in ISR trampolines).

### Weaknesses & Issues
- **Silent Failures in Generator:** `TimerCkpGenerator::apply` checks for invalid config (`nTeeth == 0`, etc.) and returns early (line 36 in `CkpGenerator.cpp`). However, it returns `void`, so the caller has no way of knowing the configuration was rejected. The UI might show "Running" while the generator is actually idle or using old settings.
- **Queue Overflow Handling:** `xQueueSend(gCtrlQ, &m, 0)` uses a 0 timeout. If the queue is full (e.g., a burst of UI events), the command is dropped silently. This could lead to a "pressed button but nothing happened" scenario.
- **Memory Allocation Failure:** If `heap_caps_malloc` fails for the LVGL buffer, the device hangs in an infinite loop. A better approach would be to try a smaller buffer or fallback to a text-only mode/error LED pattern.
- **Unchecked Return Values:** The return value of `xQueueReceive` in `managerTask` is checked, but other system calls (like `timerBegin` in older ESP32 cores, though `hw_timer_t*` is returned here) are implicitly trusted to succeed.
