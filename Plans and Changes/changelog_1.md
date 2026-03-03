# Changelog 1

## Unreleased
- Removed Serial CLI input and command parsing; device is UI-controlled.
- Added LVGL Start/Stop control and synchronized RPM/pattern controls.
- Added missing "12-1" pattern to the UI dropdown and a "CUSTOM" entry to launch the custom pattern editor.
- Added backend→UI synchronization APIs (`ui_update_rpm`, `ui_update_pattern`, `ui_update_running`) with loop-safe suppression.
- Added lightweight touch filtering (50ms + small-move threshold) to reduce noisy UI updates.
- Replaced hard `while(true)` UI init failures with `ui_init()` returning `false` and draw-buffer fallback sizing.
- Added `SignalConfig` validation and changed `TimerCkpGenerator::apply()` to return `bool`.
- Simplified generator ISR by removing modulo tick math (slot-in-period counter + precomputed gap start).
- Moved capture into `lib/ckp_capture` and switched ISR pin reads to GPIO register reads.
- Added queue/task/timer initialization checks and UI-visible error messages.
