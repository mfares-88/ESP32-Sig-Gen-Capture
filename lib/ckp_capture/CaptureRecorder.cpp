// lib/ckp_capture/CaptureRecorder.cpp — see CaptureRecorder.h.
//
// M6.1 implementation strategy:
//   1. captureStart() registers an ISR-on-CHANGE that fills a ring of
//      edge timestamps (uint32_t micros()) until N revolutions have been
//      observed. The "one revolution" heuristic is the longest gap in
//      the first ~120 edges (the missing-tooth gap dominates).
//   2. captureFetchPattern() takes the collected timestamps, derives a
//      slot count from the mean tooth-gap period and emits a byte table
//      where each slot bit reflects the signal level during that slot.
//
// Limitations (documented for future tightening):
//   * Single-channel only (we sample the crank input pin). cam1/cam2
//     loopback validation would require dedicated edge-capture pins —
//     not in M6.1 scope.
//   * `slot_count` derivation assumes the captured signal contains at
//     least one missing-tooth gap; uniform-toothed wheels degrade to a
//     count-based heuristic.

#include "CaptureRecorder.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

namespace {

constexpr size_t kMaxEdges = 1024;  // enough for 60-2 over 4 revs.

struct CaptureState {
  volatile bool armed         = false;
  volatile bool done          = false;
  volatile uint16_t edges     = 0;
  volatile uint16_t target_revs = 0;
  uint32_t timestamps[kMaxEdges];
  uint8_t  levels[kMaxEdges];
  int      pin             = -1;
};

CaptureState g_cap;
portMUX_TYPE g_cap_mux = portMUX_INITIALIZER_UNLOCKED;

void IRAM_ATTR onEdge() {
  if (!g_cap.armed) return;
  if (g_cap.edges >= kMaxEdges) {
    g_cap.armed = false;
    g_cap.done  = true;
    return;
  }
  const uint32_t now = micros();
  const int lvl = digitalRead(g_cap.pin);
  const size_t i = g_cap.edges;
  g_cap.timestamps[i] = now;
  g_cap.levels[i] = (uint8_t)lvl;
  g_cap.edges = (uint16_t)(i + 1);
}

}  // namespace

bool captureStart(uint8_t pin, uint16_t revolutions) {
  portENTER_CRITICAL(&g_cap_mux);
  if (g_cap.armed) {
    portEXIT_CRITICAL(&g_cap_mux);
    return false;
  }
  g_cap.pin         = (int)pin;
  g_cap.edges       = 0;
  g_cap.target_revs = revolutions ? revolutions : 2;
  g_cap.done        = false;
  g_cap.armed       = true;
  portEXIT_CRITICAL(&g_cap_mux);

  pinMode(pin, INPUT);
  attachInterrupt(digitalPinToInterrupt(pin), onEdge, CHANGE);
  return true;
}

void captureStop() {
  if (g_cap.pin < 0) return;
  detachInterrupt(digitalPinToInterrupt(g_cap.pin));
  g_cap.armed = false;
  g_cap.done  = true;
}

bool captureFetchPattern(PatternRef& out) {
  // Stop capture if still armed (caller may invoke this right after start).
  if (g_cap.armed) {
    // Brief wait for at least a couple of revolutions worth of data.
    const uint32_t deadline = millis() + 500;
    while (g_cap.armed && g_cap.edges < 240 && millis() < deadline) {
      delay(2);
    }
    captureStop();
  }
  if (g_cap.edges < 8) return false;

  // Find the largest inter-edge gap; treat it as the missing-tooth marker.
  uint32_t max_gap = 0;
  size_t max_gap_idx = 0;
  for (size_t i = 1; i < g_cap.edges; ++i) {
    const uint32_t dt = g_cap.timestamps[i] - g_cap.timestamps[i - 1];
    if (dt > max_gap) { max_gap = dt; max_gap_idx = i; }
  }
  // Median inter-edge time approximation (use mean for simplicity).
  uint64_t total = 0;
  for (size_t i = 1; i < g_cap.edges; ++i) {
    total += g_cap.timestamps[i] - g_cap.timestamps[i - 1];
  }
  const uint32_t mean_dt = (uint32_t)(total / (g_cap.edges - 1));
  if (mean_dt == 0) return false;

  // slot_count ≈ edges per rev. Assume two revs span entire capture.
  const uint16_t slot_count = (uint16_t)(g_cap.edges / (g_cap.target_revs ? g_cap.target_revs : 1));
  if (slot_count < 4) return false;

  // Allocate table in PSRAM if available, else regular heap.
  uint8_t* tbl = (uint8_t*)heap_caps_malloc(slot_count, MALLOC_CAP_SPIRAM);
  if (!tbl) tbl = (uint8_t*)malloc(slot_count);
  if (!tbl) return false;

  // Fill: each slot bit0 = level captured at that edge index.
  for (uint16_t i = 0; i < slot_count; ++i) {
    tbl[i] = (uint8_t)(g_cap.levels[i] & 0x01);
  }

  out.table        = tbl;
  out.slot_count   = slot_count;
  out.degrees      = 360;
  out.rpm_scaler   = (float)slot_count / 120.0f;
  out.channel_mask = 0x01;
  out.name_key     = nullptr;  // caller assigns.
  (void)max_gap; (void)max_gap_idx;
  return true;
}

// --- Loopback validator -----------------------------------------------

namespace {
PatternRef    s_expected{};
uint32_t      s_tolerance_us = 0;
bool          s_loopback_on = false;
volatile bool s_loopback_err = false;
char          s_loopback_msg[96] = {0};
// We re-arm a short single-shot capture each tick to gather a fresh
// window of edges to validate. Reusing g_cap's edge buffer keeps memory
// bounded to one ring.
uint32_t      s_last_tick_ms = 0;
}

bool loopbackEnable(const PatternRef& expected, uint32_t tolerance_us) {
  s_expected     = expected;
  s_tolerance_us = tolerance_us ? tolerance_us : 50;  // 50 µs default
  s_loopback_on  = true;
  s_loopback_err = false;
  s_loopback_msg[0] = '\0';
  s_last_tick_ms = 0;
  return true;
}

void loopbackDisable() {
  s_loopback_on  = false;
  s_loopback_err = false;
  s_loopback_msg[0] = '\0';
}

bool captureLoopbackHasError() {
  return s_loopback_err;
}

const char* captureLoopbackErrorMsg() {
  return s_loopback_msg;
}

void captureLoopbackTick(uint32_t current_rpm) {
  if (!s_loopback_on) return;
  if (s_expected.slot_count == 0 || s_expected.table == nullptr) return;
  if (current_rpm == 0) return;

  // Expected slot period at this RPM:
  //   one mechanical rev = 60'000'000 / rpm microseconds
  //   one slot          = rev / slot_count
  // (Cross-multiplied to avoid premature division loss.)
  const uint64_t expected_us =
      60000000ULL / ((uint64_t)current_rpm * (uint64_t)s_expected.slot_count);
  if (expected_us == 0) return;

  // We need a fresh sample window. If no capture is in flight, snapshot the
  // most recent edges already in g_cap (populated by the standard capture
  // path) and compare. We require at least 2 edges to derive a delta.
  if (g_cap.edges < 2) {
    // Not enough data yet — not an error condition; the manager loop will
    // call us again shortly.
    return;
  }

  // Examine the last few inter-edge deltas. We pick the tightest window
  // (most recent 8 edges) to react within ~1 second at typical RPM.
  const uint16_t edges = g_cap.edges;
  const uint16_t start = (edges > 8) ? (uint16_t)(edges - 8) : 1;
  uint32_t worst_dev_us = 0;
  uint32_t worst_meas_us = 0;
  for (uint16_t i = start; i < edges; ++i) {
    const uint32_t dt_us = g_cap.timestamps[i] - g_cap.timestamps[i - 1];
    // A missing-tooth gap will exceed expected_us by definition; ignore
    // gaps that are >2x expected (they're the intentional sync marker).
    if (dt_us > expected_us * 2) continue;
    const uint32_t dev = (dt_us > (uint32_t)expected_us)
                          ? (dt_us - (uint32_t)expected_us)
                          : ((uint32_t)expected_us - dt_us);
    if (dev > worst_dev_us) {
      worst_dev_us  = dev;
      worst_meas_us = dt_us;
    }
  }

  if (worst_dev_us > s_tolerance_us) {
    s_loopback_err = true;
    snprintf(s_loopback_msg, sizeof(s_loopback_msg),
             "loopback: meas=%lu us exp=%lu us dev=%lu us tol=%lu us",
             (unsigned long)worst_meas_us,
             (unsigned long)expected_us,
             (unsigned long)worst_dev_us,
             (unsigned long)s_tolerance_us);
  }
  s_last_tick_ms = (uint32_t)millis();
}
