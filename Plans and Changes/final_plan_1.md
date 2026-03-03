# Final Consolidated Implementation Plan

## Step 0: Branch & Documentation Setup (From Codex)
*   **Action:** Create a new branch `Feature_1_Optimize_Code` to isolate changes.
*   **Documentation:**
    *   Create `planned_changelog_1.md` to track intended changes.
    *   Create `changelog_1.md` for completed changes.
    *   Create `Readme_Update_1.md` to document the new UI-driven workflow (since Serial CLI is being removed).

## Section 1: Decommissioning Serial & CLI (Consolidated)
*Goal: Remove serial terminal control to ensure standalone operation, while preserving debug logging capability.*
1.  **Remove Interactive CLI:**
    *   In `src/main.cpp`, completely remove `pollSerialForDemo()` and all its internal string parsing logic (addressing heap fragmentation risks).
    *   Remove helper functions: `printHelp()`, `printPatterns()`, `printStatus()`, `printPrompt()`.
2.  **Retain Debug Logging:**
    *   Keep `Serial.begin()` wrapped in `#if DEBUG` for development diagnostics, but ensure the system boots and runs fully without a serial connection.
3.  **Preserve Backend Logic:**
    *   Retain the `MSG_SET_CUSTOM` message type and `CtrlMsg` struct in `main.cpp`. This keeps the *capability* for custom patterns available for a future UI implementation, even though the CLI trigger is gone.

## Section 2: Front-End Refinement (LVGL) (Consolidated)
*Goal: Polish the UI to support all backend features and ensure state synchronization.*
1.  **Sync Pattern Options:**
    *   Update `lib/ui_lvgl/ui_lvgl.cpp`: Add the missing 5th pattern ("12-1") to the dropdown options and update `on_pattern_changed` to handle index `4`.
2.  **Bidirectional State Synchronization (Best of Both):**
    *   **New API:** Implement `ui_update_rpm(uint32_t rpm)` and `ui_update_pattern(uint8_t idx)` in `lib/ui_lvgl`.
    *   **Logic:** These will be called by `main.cpp` when the backend clamps a value or changes state.
    *   **Feedback:** Add visual feedback (e.g., flash label color) when a value is clamped/updated by the backend.
3.  **Input Stability:**
    *   Implement touch debouncing in `my_touchpad_read` (ignore events <50ms apart) to prevent double-clicks.
4.  **Safe Initialization:**
    *   Replace `while(true)` loops in `ui_init` with a safe failure mode (e.g., return `false` to main, allowing main to blink an error LED or reboot).

## Section 3: Back-End Optimization & Modularization (Consolidated)
*Goal: Cleanup, optimization, and modularity.*
1.  **Modularize Capture Logic:**
    *   Move `EdgePulseCapture` class from `src/main.cpp` to new files `lib/ckp_capture/EdgePulseCapture.h` and `.cpp`.
2.  **ISR Optimization (Performance):**
    *   **Generator:** In `TimerCkpGenerator`, pre-calculate gap logic in `apply()` to simplify the `onTimer` ISR (reduce conditional branches).
    *   **Capture:** In `EdgePulseCapture::onEdgeISR()`, replace `digitalRead()` with direct GPIO register reads for lower latency.
3.  **Code Cleanup:**
    *   Remove large commented-out dead code blocks in `src/main.cpp`.

## Section 4: Error Handling & Architecture Safety (Consolidated)
*Goal: Robust closed-loop control and failure prevention.*
1.  **Config Validation:**
    *   Implement `bool validate(const SignalConfig& cfg)` in `lib/ckp_gen`.
    *   Enforce: `nTeeth > nMiss`, `pMiss > 0`, `gapSlots < slotsPerPeriod`.
    *   Update `TimerCkpGenerator::apply` to return `bool`.
2.  **Closed-Loop Feedback:**
    *   **Queue Safety:** Check `xQueueSend` return values in `main.cpp`. Log/count overflows.
    *   **Validation Loop:** If `genTX.apply()` returns `false`, `managerTask` must revert to the last valid config and notify the UI to undo the user's change.
3.  **Critical Init Checks:**
    *   Add success checks for `xQueueCreate`, `xTaskCreatePinnedToCore`, and timer init. Fail gracefully (safe mode) if these key resources cannot be allocated.

## Execution Order
1.  **Setup:** Branch & Docs.
2.  **Phase 1 (Core):** Modularize Capture & Remove CLI (clean slate).
3.  **Phase 2 (UI/Sync):** Implement UI updates, Sync API, and Touch Debounce.
4.  **Phase 3 (Robustness):** Implement ISR optimizations, Validation logic, and Safety checks.
