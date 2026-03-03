# Planned Changelog 1

## Serial Interface Removal
- Remove serial CLI input parsing and command handling from src/main.cpp.
- Remove serial-driven state synchronization paths and rely on LVGL UI callbacks.
- Keep optional Serial debug logging only if needed for diagnostics.
- Verify generator control (start/stop, RPM, pattern, custom config) is fully UI-driven.

## Front End (LVGL UI)
- Add missing 12-1 pattern option to UI dropdown.
- Add UI synchronization API for running state and pattern selection.
- Provide visual feedback when RPM values are clamped to min/max.
- Replace init infinite loops with recoverable error screen/fallback mode.
- Add lightweight touch filtering to reduce noisy updates.

## Back End (Control Logic, ISR)
- Implement validateConfig() and enforce gap/math constraints before applying config.
- Change TimerCkpGenerator::apply to return bool; surface failures to UI.
- Replace String parsing with fixed buffers if any residual parsing remains.
- Improve ISR capture timing by using direct GPIO reads or plan PCNT/RMT.
- Check queue send results and add overflow counters for diagnostics.

## Error Handling
- Add checks for queue/task creation and timer setup; handle failures safely.
- Avoid hard infinite loops on errors; keep system in safe mode.
- Ensure UI reflects failed apply() or invalid configuration attempts.
- Ensure status/acknowledgement reflects success or failure.
