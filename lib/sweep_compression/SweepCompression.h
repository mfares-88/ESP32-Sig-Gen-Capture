// lib/sweep_compression/SweepCompression.h
//
// Sweep + compression simulation task.
//
// Implements §3.6 of _Plans-and-Records/implementation_plan.md. Scaffolded at
// M0.3 (interface freeze) — body lands at M4.1–M4.4. Compression behavior
// targets bit-equivalence with References/ardustim.ino:302-389 including the
// AVR `base_rpm < 655U` overflow-guard gate (preserved for parity, per §8 #5).
//
// Owner: Agent C (sweep-store).

#pragma once

#include <stdint.h>

// Forward-declared so this header doesn't drag the generator backend into
// every translation unit that just wants the SweepConfig type.
struct IGenerator;

#ifdef __cplusplus
extern "C" {
#endif

enum SweepMode : uint8_t {
    SWEEP_OFF       = 0,
    SWEEP_LINEAR    = 1,
    SWEEP_LOG       = 2,
    SWEEP_WAYPOINT  = 3,
};

struct SweepConfig {
    uint16_t        low_rpm;
    uint16_t        high_rpm;
    SweepMode       mode;
    uint32_t        interval_us;       // µs per ±1 RPM step (linear); dwell base (log/waypoint)
    const uint32_t* waypoints;         // SWEEP_WAYPOINT: packed (rpm, dwell_ms) pairs
    uint8_t         waypoint_count;    // number of (rpm, dwell_ms) pairs
};

struct CompressionConfig {
    bool            enabled;
    uint8_t         cyl;               // 1, 2, 3, 4 stroke profile selector
    uint16_t        rpm_thresh;        // compression effect cuts out above this RPM
    uint8_t         peak;              // peak modifier amplitude (Ardu-Stim default: 100)
    bool            dynamic;           // dynamic compression mode (uses 655U guard)
    uint16_t        offset_deg;        // phase offset in crank degrees
    const uint8_t*  custom_curve_256;  // optional user-supplied 256-entry curve (replaces sin LUT)
};

// One-shot init: spins up the FreeRTOS task at priority 2 on Core 1 (per §6
// Agent C hard rules) and stashes the generator pointer for setRpm() calls.
// Safe to call before scheduler start; returns false on alloc failure.
bool      sweepCompressionInit(IGenerator* gen);

// Atomic update of the active sweep / compression configs. Both are
// copy-by-value; pointer members (waypoints / custom_curve_256) must outlive
// the next call to sweepSet/compressionSet that replaces them.
void      sweepSet(const SweepConfig& cfg);
void      compressionSet(const CompressionConfig& cfg);

// Tell the sweep/compression task what "base RPM" to ride compression on
// top of when SWEEP_OFF is selected. The manager task calls this from its
// MSG_SET_RPM handler so the compression simulator can dip below the
// user's chosen RPM even with no sweep active. No-op when sweep is in
// LINEAR/LOG/WAYPOINT mode — those modes drive s_base_rpm themselves.
void      sweepSetBaseRpm(uint32_t rpm);

// Current swept RPM — for UI display and the LVGL pending-flag sync. Cheap
// 32-bit aligned read on Xtensa, no critical section.
uint32_t  sweepCurrentRpm();

#ifdef __cplusplus
}
#endif
