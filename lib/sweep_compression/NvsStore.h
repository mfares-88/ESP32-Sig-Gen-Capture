// lib/sweep_compression/NvsStore.h
//
// M3.5 (Agent C, sweep-store): NVS schema v1 — `siggen` namespace.
//
// Persistent configuration for the signal generator. Loaded on boot,
// rewritten on every "apply" event. Implements the persistence half of
// §3.6 / §6 Agent C standing orders in
// _Plans-and-Records/implementation_plan.md:
//
//   * Schema is versioned via `schema_version` (uint8). Mismatches default
//     to factory settings and NEVER block boot.
//   * Setters commit immediately; getters return false when the key is
//     missing so the caller can substitute a sane default.
//   * Available on both `esp32-s3-n4r8` and `esp32-wroom32d` envs — NVS is
//     part of the ESP32 standard partition layout, no LittleFS dependency.
//
// Globals (declared `extern` here, defined in NvsStore.cpp): these are the
// single canonical mirror of NVS state that the rest of the firmware
// (main.cpp, managerTask, UI, sweep_compression task) reads. After
// `NvsStore::loadAllToGlobals()` they hold either the persisted values or
// the factory defaults.
//
// Note (M4 forward-compat): the AVR `compressionDynamic < 655U` overflow
// guard from References/ardustim.ino:372 is documented in compressionSet()
// in SweepCompression.cpp — NvsStore just stores the `dynamic` bool, it
// does not gate on it.
//
// Owner: Agent C (sweep-store). Consumed by Agent E in src/main.cpp.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace NvsStore {

// Bump when keys are added/removed/retyped so older firmware on flash
// migrates cleanly (or factory-resets, never blocks boot).
static constexpr uint8_t SCHEMA_VERSION = 1;

// Factory defaults — used when NVS is empty / mismatched. Kept inline here
// so the values are visible at the API surface (no hunting through .cpp).
static constexpr const char* DEFAULT_PATTERN_KEY = "sixty_minus_two";
static constexpr uint32_t    DEFAULT_RPM         = 1000;
static constexpr uint8_t     DEFAULT_INVERT_MASK = 0;

static constexpr uint16_t DEFAULT_SWEEP_LOW_RPM     = 500;
static constexpr uint16_t DEFAULT_SWEEP_HIGH_RPM    = 5000;
static constexpr uint8_t  DEFAULT_SWEEP_MODE        = 0;       // SWEEP_OFF
static constexpr uint32_t DEFAULT_SWEEP_INTERVAL_US = 1000;

static constexpr bool     DEFAULT_COMP_ENABLED    = false;
static constexpr uint8_t  DEFAULT_COMP_CYL        = 4;
static constexpr uint16_t DEFAULT_COMP_RPM_THRESH = 655;       // matches AVR guard
static constexpr uint8_t  DEFAULT_COMP_PEAK       = 100;       // Ardu-Stim default amplitude
static constexpr bool     DEFAULT_COMP_DYNAMIC    = false;

// Max pattern_key length (matches PatternRef.name_key budget — Agent B's
// generator keeps keys well under 64 bytes).
static constexpr size_t PATTERN_KEY_BUFLEN = 64;

// ---- Lifecycle ----------------------------------------------------------

// Initialize nvs_flash (once globally) and open the `siggen` namespace.
// Performs schema-version check + migration. Safe to call multiple times.
// Returns true if NVS is open and usable.
bool begin();

// True after a successful begin().
bool isReady();

// ---- Pattern selection (string key) -------------------------------------

bool setPatternKey(const char* key);
// Reads into `buf`; returns false if the key is missing or buf is too small.
bool getPatternKey(char* buf, size_t buflen);

// ---- RPM ----------------------------------------------------------------

bool setRpm(uint32_t rpm);
bool getRpm(uint32_t* out);

// ---- Invert mask --------------------------------------------------------

bool setInvertMask(uint8_t mask);
bool getInvertMask(uint8_t* out);

// ---- Sweep config -------------------------------------------------------

bool setSweep(uint16_t low_rpm, uint16_t high_rpm, uint8_t mode,
              uint32_t interval_us);
bool getSweep(uint16_t* low_rpm, uint16_t* high_rpm, uint8_t* mode,
              uint32_t* interval_us);

// ---- Compression config -------------------------------------------------

bool setCompression(bool enabled, uint8_t cyl, uint16_t rpm_thresh,
                    uint8_t peak, bool dynamic);
bool getCompression(bool* enabled, uint8_t* cyl, uint16_t* rpm_thresh,
                    uint8_t* peak, bool* dynamic);

// ---- Bulk helpers -------------------------------------------------------

// Populate the `g_*` globals below from NVS. Missing keys silently fall
// back to the DEFAULT_* constants above. Idempotent; safe to call again
// after a factory reset to repopulate globals.
//
// Acceptance criterion for M3.5 (per implementation_plan.md row M3.5):
// after a reboot, `g_pattern_key` reflects the last-applied key — provided
// the caller invoked setPatternKey() before the reboot.
void loadAllToGlobals();

// Saver counterpart used by the manager task: writes the current globals
// back to NVS in one shot. Individual setters already commit, so this is
// only needed for "save now" batch operations (e.g. an explicit UI button
// or migration). Setters above are the primary save path.
void saveAllFromGlobals();

}  // namespace NvsStore

// ---- Canonical global state mirror --------------------------------------
//
// Defined in NvsStore.cpp. After NvsStore::begin() + loadAllToGlobals(),
// these hold the live configuration. main.cpp, managerTask and the
// sweep/compression task all read from these.

extern char     g_pattern_key[NvsStore::PATTERN_KEY_BUFLEN];
extern uint32_t g_rpm;
extern uint8_t  g_invert_mask;

extern uint16_t g_sweep_low_rpm;
extern uint16_t g_sweep_high_rpm;
extern uint8_t  g_sweep_mode;
extern uint32_t g_sweep_interval_us;

extern bool     g_comp_enabled;
extern uint8_t  g_comp_cyl;
extern uint16_t g_comp_rpm_thresh;
extern uint8_t  g_comp_peak;
extern bool     g_comp_dynamic;
