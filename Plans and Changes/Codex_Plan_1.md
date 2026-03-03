# Codex Plan 1

## Step 0: Branch and Documentation Setup
- Create a new branch from main named Feature_1_Optimize_Code.
- Create/update documentation artifacts for this effort:
  - Create or update the change log file (changelog_1.md) with entries per section below.
  - Create planned_changelog_1.md to track intended changes before implementation.
  - Create Readme_Update_1.md to document UI-only control workflow after serial removal.

## Section 1: Serial Interface Removal (Testing-Only CLI)
- Identify all serial input paths and command handlers in src/main.cpp (CLI parser, serial poll loop, CLI help/status output).
- Remove serial input processing and CLI command registration while keeping Serial available for debug logs if needed.
- Ensure all control flows previously triggered by CLI are accessible via LVGL UI callbacks and queue messages.
- Update any references that synchronized UI state based on CLI changes to rely on internal state updates only.
- Validate there is no dependency on serial input for generator start/stop, RPM, pattern selection, or custom config.
- Document the removal in planned_changelog_1.md and changelog_1.md.

## Section 2: Front End (LVGL UI) Review Actions
- Add the missing 12-1 pattern entry in the UI dropdown to match backend pattern list.
- Introduce UI-to-backend state synchronization methods in lib/ui_lvgl/ui_lvgl.h/.cpp (pattern selection, running state) and call them from src/main.cpp when state changes.
- Add UI feedback for RPM clamping (min/max indicators or status label update) when the backend clamps values.
- Replace hard init failure loops with a recoverable error screen or safe fallback mode while keeping UI loop alive.
- Implement lightweight touch filtering to reduce repeated identical point updates within a short time window.
- Update planned_changelog_1.md and changelog_1.md with these UI improvements.

## Section 3: Back End (Control Logic, ISR, CLI) Review Actions
- Add a centralized validateConfig() used by managerTask() and UI-triggered config changes before genTX.apply().
- Update TimerCkpGenerator::apply to return bool and propagate failures up to callers for UI feedback.
- Replace String-based parsing with fixed-size buffers and strtol/strtok only if any residual parsing remains after serial removal.
- Replace digitalRead() in ISR with direct GPIO register reads, or plan a migration to PCNT/RMT for capture timing.
- Check queue send return values and track overflow counters for diagnostics (log via debug if enabled).
- Document backend changes in planned_changelog_1.md and changelog_1.md.

## Section 4: Error Handling Review Actions
- Add explicit success checks for critical initialization calls (xQueueCreate, xTaskCreatePinnedToCore, timerBegin, timerAttachInterrupt).
- Route failures to a safe fallback state and log errors for diagnosis; avoid infinite loops.
- Ensure apply() failures are reported to UI and any status displays.
- Ensure all command acknowledgements (now UI-triggered) reflect success or failure accurately.
- Update planned_changelog_1.md and changelog_1.md with error handling improvements.
