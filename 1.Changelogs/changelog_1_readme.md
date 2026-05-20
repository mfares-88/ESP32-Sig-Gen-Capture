# Readme Update 1

## UI-Only Control Workflow
- The device is controlled exclusively through the LVGL UI (no Serial CLI).
- UI controls:
  - RPM via the arc (100–6000).
  - Pattern via the dropdown (60-2, 36-1, 36-2, 36-1-1, 12-1, CUSTOM).
  - Custom patterns via the CUSTOM editor (teeth/periods/missing teeth, gap position, gap level).
  - Start/Stop via the on-screen button.
- All UI actions send messages through a FreeRTOS queue to the manager task, which applies changes to the generator.

## State Synchronization
- The backend can push state back to the UI via `ui_update_rpm()`, `ui_update_pattern()`, and `ui_update_running()`.
- Backend-driven RPM updates flash the RPM label briefly to indicate an applied change.

## Error Handling
- UI initialization returns `false` on failures (display init or draw-buffer allocation) instead of hard-locking.
- If queue/task creation fails, an error message is shown on-screen and the system continues running defaults.

## Debugging
- Serial output can still be enabled for debug logs via `DEBUG` in `src/main.cpp`.
