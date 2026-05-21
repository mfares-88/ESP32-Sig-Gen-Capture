// lib/sweep_compression/SweepCompression.cpp
//
// M4.1 + M4.2 + M4.3 + M4.4 implementation. See SweepCompression.h for the
// public contract and implementation_plan.md §3.6 / §M4 / §6 (Agent C) for
// the design intent.
//
// Single FreeRTOS task on Core 1, priority 2 (one below the manager task
// at priority 3 — see src/main.cpp xTaskCreatePinnedToCore call). The task
// interleaves sweep stepping and compression modulation; both run cheaply
// (single LUT read + a few multiplies) so keeping them in one task avoids
// any cross-task synchronization for the "base RPM" that compression rides
// on top of.
//
// Hard-rule compliance (per §6 Agent C):
//   * Task priority 2, pinned to Core 1.
//   * Sweep + compression call gen->setRpm() only — never apply() — so the
//     backend never rebuilds buffers in this hot path.
//   * AVR `base_rpm < 655U` dynamic-compression overflow guard preserved
//     verbatim for behavioral parity (see calculateCompressionModifier
//     below, comment cites References/ardustim.ino:372).

#include "SweepCompression.h"
#include "CompressionTables.h"

#include <math.h>          // powf for SWEEP_LOG
#include <stdint.h>
#include <string.h>

#include <Arduino.h>       // Serial logging (debug only)

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "CkpGenerator.h"  // IGenerator (with getCycleStartUs/getCycleDurationUs)
#include "esp_timer.h"     // esp_timer_get_time

// ---------------------------------------------------------------------------
// Task-local state
// ---------------------------------------------------------------------------

namespace {

// Generator handle stashed by sweepCompressionInit().
IGenerator* s_gen = nullptr;

// Active configs (double-checked via mutex on writes; reads under mutex).
SweepConfig s_sweep_cfg = {
    /*low_rpm*/      500,
    /*high_rpm*/     5000,
    /*mode*/         SWEEP_OFF,
    /*interval_us*/  1000,
    /*waypoints*/    nullptr,
    /*waypoint_count*/0,
};

CompressionConfig s_comp_cfg = {
    /*enabled*/         false,
    /*cyl*/             4,
    /*rpm_thresh*/      655,
    /*peak*/            100,
    /*dynamic*/         false,
    /*offset_deg*/      0,
    /*custom_curve_256*/nullptr,
};

// Sweep working state. `s_base_rpm` is the "swept" RPM before compression
// modulation; `s_current_rpm` is the actual value last fed to setRpm()
// (post-compression). Both are uint32_t volatile for cheap aligned reads.
volatile uint32_t s_base_rpm    = 1000;
volatile uint32_t s_current_rpm = 1000;

// Linear sweep state — direction flag and last-step timestamp, mirroring
// References/ardustim.ino:302-318.
enum SweepDir : uint8_t { ASCENDING = 0, DESCENDING = 1 };
uint8_t  s_sweep_dir          = ASCENDING;
uint32_t s_sweep_time_counter = 0;

// SWEEP_LOG state — start timestamp of the current half-ramp.
uint32_t s_log_phase_start_us = 0;
bool     s_log_ascending      = true;

// SWEEP_WAYPOINT state — index of the active segment plus its start time.
size_t   s_wp_segment      = 0;
uint32_t s_wp_segment_start_us = 0;

SemaphoreHandle_t s_cfg_mux = nullptr;

// FreeRTOS task handle (so we don't double-spawn).
TaskHandle_t s_task_handle = nullptr;

inline void cfgLock()   { if (s_cfg_mux) xSemaphoreTake(s_cfg_mux, portMAX_DELAY); }
inline void cfgUnlock() { if (s_cfg_mux) xSemaphoreGive(s_cfg_mux); }

inline uint32_t nowUs() { return (uint32_t)esp_timer_get_time(); }

// ---------------------------------------------------------------------------
// Crank-angle math — port of References/ardustim.ino:377-389.
//
// AVR version doubles cycleTime for 720° patterns because cycleDuration
// reflects one revolution. Our TableCkpGenerator publishes cycleDuration
// over the *full pattern* (one 720° wrap), so we use 720° as the modulus
// directly when the pattern is 4-stroke. Callers that don't know the
// degree count (the dispatcher below) work in a 0..360 angle space and
// fold 4-stroke patterns to a half-cycle, which matches the AVR behavior
// for the 4cyl/6cyl/8cyl branches that mod-down to 180/120/90.
// ---------------------------------------------------------------------------

// Returns crank angle in 0..359 degrees (with offset folded in). Returns
// UINT16_MAX if the cycle timing is not yet published (cycleDuration == 0),
// signalling "no signal — skip compression".
uint16_t currentCrankAngle(uint16_t offset_deg) {
    if (!s_gen) return 0xFFFFu;
    const uint32_t cycle_dur = s_gen->getCycleDurationUs();
    if (cycle_dur == 0) return 0xFFFFu;

    const uint32_t cycle_start = s_gen->getCycleStartUs();
    uint32_t cycle_time = nowUs() - cycle_start;
    // Clamp against rollover during the rare instant when the ISR is
    // mid-update of (cycle_start, cycle_duration).
    if (cycle_time > cycle_dur) cycle_time = cycle_dur;

    uint16_t angle = (uint16_t)((cycle_time * 360UL) / cycle_dur);
    angle += offset_deg;
    while (angle >= 360) angle -= 360;
    return angle;
}

// ---------------------------------------------------------------------------
// Compression modifier — port of References/ardustim.ino:333-389.
//
// Returns a positive delta to subtract from base_rpm. Honors:
//   * `enabled` — global on/off.
//   * `rpm_thresh` — disables when base_rpm > rpm_thresh (matches
//     `currentStatus.base_rpm > config.compressionRPM` in AVR:335).
//   * `cyl` — selects sin LUT (1→180, 2→180/2-mapped, 3→120, 4+→90).
//     Even cyl counts collapse to the 90° table (matches AVR
//     COMPRESSION_TYPE_8CYL_4STROKE / default).
//   * `peak` — replaces the hardcoded amplitude of 100 (M4.4).
//   * `custom_curve_256` — when non-null, overrides the sin LUT with a
//     user-provided 256-entry curve indexed 0..255 across one crank cycle.
//   * `dynamic` — when set AND base_rpm < 655U (AVR overflow guard, see
//     References/ardustim.ino:372 — preserved per §8 risk #5), scale the
//     modifier amplitude by (base_rpm / rpm_thresh).
// ---------------------------------------------------------------------------

uint16_t calculateCompressionModifier(uint32_t base_rpm,
                                      const CompressionConfig& comp) {
    if (!comp.enabled) return 0;
    if (base_rpm > comp.rpm_thresh) return 0;

    const uint16_t angle = currentCrankAngle(comp.offset_deg);
    if (angle == 0xFFFFu) return 0;   // no cycle timing published yet

    uint16_t modifier = 0;

    if (comp.custom_curve_256 != nullptr) {
        // Custom 256-entry curve mapped across 0..359 degrees.
        // idx = (angle * 256) / 360, max 255.
        const uint16_t idx = (uint16_t)((uint32_t)angle * 256U / 360U);
        const uint8_t  v   = comp.custom_curve_256[idx > 255 ? 255 : idx];
        modifier = v;
    } else {
        // Sin-LUT dispatch — chooses table by cylinder count (M4.4).
        // 1-cyl / 2-cyl: a single combustion event per 720° = use the
        // 180°-cycle table folded over the (0..359) angle window. For
        // 1-cyl we sample once per rev (mod 360→/2); for 2-cyl we mod
        // 180 (matches AVR COMPRESSION_TYPE_2CYL_4STROKE: modAngle =
        // crankAngle / 2 on a 720° base, equivalent to crankAngle/2 on
        // a 360° base when the table is doubled).
        switch (comp.cyl) {
            case 1: {
                // 1-cyl: one full sin per 360° → angle/2 indexes the 180-table.
                const uint16_t i = (uint16_t)(angle / 2);
                modifier = SweepCompressionTables::sin_100_180[i % 180];
                break;
            }
            case 2: {
                // 2-cyl: matches AVR COMPRESSION_TYPE_2CYL_4STROKE
                // (modAngle = crankAngle / 2 over the 720° AVR scale).
                // On a 360° crank the equivalent map is angle / 2.
                const uint16_t i = (uint16_t)(angle / 2);
                modifier = SweepCompressionTables::sin_100_180[i % 180];
                break;
            }
            case 3: {
                // 3-cyl: one combustion per 120° crank rotation.
                const uint16_t i = (uint16_t)(angle % 120);
                modifier = SweepCompressionTables::sin_100_120[i];
                break;
            }
            case 4:
            default: {
                // 4-cyl and even-cyl default: AVR maps to sin_100_90
                // for 8cyl (90°/comp) and sin_100_180 for 4cyl (180°/comp).
                // The reference 4cyl branch (ardustim.ino:348-351) uses
                // sin_100_180 with crankAngle % 180. We honor that for 4
                // and fall through to 90° table for 6-and-higher even.
                if (comp.cyl <= 4) {
                    const uint16_t i = (uint16_t)(angle % 180);
                    modifier = SweepCompressionTables::sin_100_180[i];
                } else {
                    const uint16_t i = (uint16_t)(angle % 90);
                    modifier = SweepCompressionTables::sin_100_90[i];
                }
                break;
            }
        }
    }

    // M4.4: scale by `peak` (0..100) — replaces hardcoded amplitude.
    if (comp.peak != 100 && comp.peak <= 100) {
        modifier = (uint16_t)(((uint32_t)modifier * comp.peak) / 100U);
    }

    // AVR dynamic-compression scaler (References/ardustim.ino:372):
    //   if(config.compressionDynamic && (currentStatus.base_rpm < 655U)) {
    //     compressionModifier = (compressionModifier * currentStatus.base_rpm)
    //                           / config.compressionRPM;
    //   }
    // The `< 655U` clause is an AVR overflow guard (uint16 * uint16
    // promotion ceiling). ESP32 doesn't need it but we preserve the gate
    // for behavioral parity — per §8 risk #5 and §6 Agent C hard rules.
    if (comp.dynamic && base_rpm < 655U && comp.rpm_thresh > 0) {
        modifier = (uint16_t)(((uint32_t)modifier * base_rpm) / comp.rpm_thresh);
    }

    return modifier;
}

// ---------------------------------------------------------------------------
// Sweep step — advances `s_base_rpm` per the active SweepConfig.
// Returns true if `s_base_rpm` changed and a setRpm() call is warranted.
// ---------------------------------------------------------------------------

bool sweepStepLinear(const SweepConfig& cfg) {
    // Verbatim port of References/ardustim.ino:302-318. Note the AVR
    // uses `micros()` directly; we substitute esp_timer_get_time() which
    // also wraps after ~71 minutes but unsigned diff math handles that.
    const uint32_t now = nowUs();
    if ((uint32_t)(now - s_sweep_time_counter) < cfg.interval_us) {
        return false;
    }
    s_sweep_time_counter = now;

    uint32_t tmp = s_base_rpm;
    if (s_sweep_dir == ASCENDING) {
        tmp = tmp + 1;
        if (tmp >= cfg.high_rpm) { s_sweep_dir = DESCENDING; }
    } else {
        tmp = (tmp > 0) ? (tmp - 1) : 0;
        if (tmp <= cfg.low_rpm) { s_sweep_dir = ASCENDING; }
    }
    s_base_rpm = tmp;
    return true;
}

bool sweepStepLog(const SweepConfig& cfg) {
    // Exponential ramp: rpm = low * (high/low)^(t/period_us). Period_us is
    // taken from cfg.interval_us — one full half-ramp covers `interval_us`
    // microseconds, after which we flip direction (so a "round trip" is
    // 2 * interval_us). powf is fine — the ESP32 has hardware FP.
    if (cfg.low_rpm == 0 || cfg.high_rpm == 0 || cfg.high_rpm == cfg.low_rpm) {
        return false;
    }
    const uint32_t now = nowUs();
    const uint32_t period_us = cfg.interval_us == 0 ? 1U : cfg.interval_us;
    const uint32_t elapsed = now - s_log_phase_start_us;

    if (elapsed >= period_us) {
        // Flip direction and reset phase.
        s_log_ascending = !s_log_ascending;
        s_log_phase_start_us = now;
        s_base_rpm = s_log_ascending ? cfg.low_rpm : cfg.high_rpm;
        return true;
    }

    const float t = (float)elapsed / (float)period_us;
    const float lo = s_log_ascending ? (float)cfg.low_rpm  : (float)cfg.high_rpm;
    const float hi = s_log_ascending ? (float)cfg.high_rpm : (float)cfg.low_rpm;
    const float ratio = hi / lo;
    const float rpm = lo * powf(ratio, t);
    const uint32_t r = (uint32_t)(rpm + 0.5f);
    if (r == s_base_rpm) return false;
    s_base_rpm = r;
    return true;
}

bool sweepStepWaypoint(const SweepConfig& cfg) {
    // Waypoint format: cfg.waypoints[] is a packed array of (rpm, dwell_ms)
    // uint32 pairs, length cfg.waypoint_count entries (i.e. 2 * count
    // uint32 words). Linear interp from waypoint[i] to waypoint[i+1] over
    // `dwell_ms` milliseconds. Wraps back to 0 after the last segment.
    if (cfg.waypoints == nullptr || cfg.waypoint_count < 2) return false;

    const uint32_t now = nowUs();
    const uint32_t* wp = cfg.waypoints;
    size_t seg = s_wp_segment;
    if (seg >= cfg.waypoint_count) seg = 0;
    const size_t next = (seg + 1u) % cfg.waypoint_count;

    const uint32_t rpm_a    = wp[seg * 2u + 0u];
    const uint32_t dwell_ms = wp[seg * 2u + 1u];
    const uint32_t rpm_b    = wp[next * 2u + 0u];
    const uint32_t dwell_us = dwell_ms * 1000U;

    if (dwell_us == 0) {
        s_wp_segment = next;
        s_wp_segment_start_us = now;
        s_base_rpm = rpm_b;
        return true;
    }

    const uint32_t elapsed = now - s_wp_segment_start_us;
    if (elapsed >= dwell_us) {
        s_wp_segment = next;
        s_wp_segment_start_us = now;
        s_base_rpm = rpm_b;
        return true;
    }

    // Linear interp.
    const int32_t  delta = (int32_t)rpm_b - (int32_t)rpm_a;
    const int64_t  add   = (int64_t)delta * (int64_t)elapsed / (int64_t)dwell_us;
    const uint32_t r     = (uint32_t)((int32_t)rpm_a + (int32_t)add);
    if (r == s_base_rpm) return false;
    s_base_rpm = r;
    return true;
}

// ---------------------------------------------------------------------------
// Task body
// ---------------------------------------------------------------------------

void sweepCompressionTask(void* /*arg*/) {
    // Tick every 1 ms — fast enough to keep the linear sweep responsive
    // even at interval_us = 1000 (1 step per ms = 60 RPM/s, matching
    // the Ardu-Stim default). The compression LUT is cheap so we re-run
    // it every tick.
    const TickType_t tick_period = pdMS_TO_TICKS(1);
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last_wake, tick_period);

        SweepConfig       sweep;
        CompressionConfig comp;
        cfgLock();
        sweep = s_sweep_cfg;
        comp  = s_comp_cfg;
        cfgUnlock();

        if (!s_gen) continue;
        if (sweep.mode == SWEEP_OFF && !comp.enabled) {
            // Idle — nothing to feed to the generator.
            continue;
        }

        // --- Sweep phase ---
        bool base_changed = false;
        switch (sweep.mode) {
            case SWEEP_LINEAR:   base_changed = sweepStepLinear(sweep); break;
            case SWEEP_LOG:      base_changed = sweepStepLog(sweep);    break;
            case SWEEP_WAYPOINT: base_changed = sweepStepWaypoint(sweep); break;
            case SWEEP_OFF:
            default: break;
        }

        // --- Compression phase ---
        // Mirrors References/ardustim.ino:325-330:
        //   currentStatus.base_rpm = tmp_rpm;
        //   compressionModifier = calculateCompressionModifier();
        //   if(compressionModifier >= base_rpm) { compressionModifier = 0; }
        //   setRPM(base_rpm - compressionModifier);
        const uint32_t base = s_base_rpm;
        uint16_t mod = comp.enabled
                            ? calculateCompressionModifier(base, comp)
                            : 0;
        if (mod >= base) mod = 0;
        const uint32_t target = base - mod;

        // Only push to the generator when something changed — avoids
        // flooding the gptimer reprogram path. setRpm() is the fast path
        // (no buffer rebuild — per §6 Agent C hard rules).
        if (target != s_current_rpm || base_changed || comp.enabled) {
            if (target >= 10) {
                s_gen->setRpm(target);
                s_current_rpm = target;
            }
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool sweepCompressionInit(IGenerator* gen) {
    if (gen == nullptr) return false;
    s_gen = gen;

    if (s_cfg_mux == nullptr) {
        s_cfg_mux = xSemaphoreCreateMutex();
        if (!s_cfg_mux) return false;
    }

    if (s_task_handle != nullptr) {
        return true;  // already spawned
    }

    // Priority 2 (one below the manager task at priority 3 — see
    // src/main.cpp xTaskCreatePinnedToCore(... "managerTask" ..., 3 ..., 1)).
    // Pinned to Core 1 to keep the LVGL renderer on Core 0 undisturbed.
    const BaseType_t ok = xTaskCreatePinnedToCore(
        sweepCompressionTask,
        "sweepCompTask",
        4096,
        nullptr,
        /*priority=*/ 2,
        &s_task_handle,
        /*core_id=*/ 1);

    if (ok != pdPASS) {
        s_task_handle = nullptr;
        return false;
    }

    s_sweep_time_counter   = nowUs();
    s_log_phase_start_us   = s_sweep_time_counter;
    s_wp_segment_start_us  = s_sweep_time_counter;
    return true;
}

void sweepSet(const SweepConfig& cfg) {
    cfgLock();
    s_sweep_cfg = cfg;
    // Reset sweep working state so a fresh config starts from a known place.
    if (cfg.mode == SWEEP_LINEAR || cfg.mode == SWEEP_LOG ||
        cfg.mode == SWEEP_WAYPOINT) {
        if (s_base_rpm < cfg.low_rpm)  s_base_rpm = cfg.low_rpm;
        if (s_base_rpm > cfg.high_rpm) s_base_rpm = cfg.high_rpm;
        s_sweep_dir           = ASCENDING;
        s_sweep_time_counter  = nowUs();
        s_log_phase_start_us  = s_sweep_time_counter;
        s_log_ascending       = true;
        s_wp_segment          = 0;
        s_wp_segment_start_us = s_sweep_time_counter;
    }
    cfgUnlock();
}

void compressionSet(const CompressionConfig& cfg) {
    cfgLock();
    s_comp_cfg = cfg;
    cfgUnlock();
}

void sweepSetBaseRpm(uint32_t rpm) {
    // Only honor when SWEEP_OFF — active sweep modes own s_base_rpm.
    cfgLock();
    const SweepMode mode = s_sweep_cfg.mode;
    cfgUnlock();
    if (mode == SWEEP_OFF && rpm >= 10) {
        s_base_rpm = rpm;
    }
}

uint32_t sweepCurrentRpm() {
    // Naturally aligned 32-bit volatile read — atomic on Xtensa.
    return s_current_rpm;
}
