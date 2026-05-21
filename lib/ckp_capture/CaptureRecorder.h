// lib/ckp_capture/CaptureRecorder.h — M6 (Agent E).
//
// High-level capture-to-table API for the loopback validator and the
// "capture as user pattern" UI flow. See implementation_plan.md §3.7.
//
//   bool captureStart(uint8_t pin, uint16_t revolutions)
//     - Arms an ISR on the given pin, records all edges for `revolutions`
//       full crank periods, then stops and prepares a PatternRef.
//   bool captureFetchPattern(PatternRef& out)
//     - Valid after captureStart completes. The returned .table is
//       allocated in PSRAM (or DRAM on builds without PSRAM); caller
//       takes ownership and must heap_caps_free() it (or register with
//       PatternLibrary which will keep it alive).
//   bool loopbackEnable(const PatternRef& expected, uint32_t tolerance_us)
//     - Continuously compares the captured edge period/duty against the
//       expected reference at the currently-applied RPM; flags divergence
//       via captureLoopbackHasError() which the manager task may poll
//       and surface to the UI.
//   void loopbackDisable()
//
// Implementation detail: we underuse the existing EdgePulseCapture (which
// only yields one CaptureReport at a time) — for M6.1 the recorder
// installs its own interrupt handler that stores raw edge timestamps
// into a per-rev ring buffer and post-processes them into a slot table.

#pragma once

#include <stdint.h>

#include "PatternRef.h"

bool captureStart(uint8_t pin, uint16_t revolutions);
bool captureFetchPattern(PatternRef& out);
void captureStop();

bool loopbackEnable(const PatternRef& expected, uint32_t tolerance_us);
void loopbackDisable();
bool captureLoopbackHasError();

// Periodic poll for the loopback validator. Call from the manager task
// (~100 ms cadence). When enabled, compares the most recent captured
// inter-edge deltas against the expected pattern's slot period at the
// current g_rpm. Sets a sticky error flag (queryable via
// captureLoopbackHasError) on tolerance breach.
//
// `current_rpm` is the live RPM the generator is producing; usually the
// caller passes `g_rpm` (NvsStore extern).
void captureLoopbackTick(uint32_t current_rpm);

// Returns a pointer to a static, NUL-terminated diagnostic message
// describing the most recent loopback failure (or "" if no failure has
// been recorded since loopbackEnable). Never returns null.
const char* captureLoopbackErrorMsg();
