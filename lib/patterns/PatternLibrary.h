// PatternLibrary — registry of builtin + user PatternRef instances.
//
// Implementation contract: implementation_plan.md §3.4 (authored by Agent B,
// consumed by Agents C/D/E). Three tiers are visible to callers:
//
//   builtin   : compiled-in patterns from References/wheel_defs.h, emitted by
//               tools/convert_ardustim_wheels.py into builtin_tables_generated.h.
//               Backed by static const PatternRef builtin_patterns[].
//   user      : runtime-registered patterns (e.g. captured waveforms, DSL
//               compilations the user chose to keep). Capacity is fixed for
//               M1.2 (kUserCapacity); future milestones may grow it.
//   scratch   : reserved tier for transient unsaved patterns. Not implemented
//               in M1.2 — findByKey() treats it as empty.
//
// Lifetime rules (per PatternRef.h):
//   - PatternRef.table for builtin entries lives in .rodata; never freed.
//   - PatternRef.table for user entries MUST be owned by the caller and
//     outlive the registration. PatternLibrary stores PatternRef by value
//     but does NOT take ownership of .table or .name_key.
//   - .name_key for user entries MUST also have static or caller-managed
//     lifetime equal to or exceeding the registration lifetime.
//
// Thread-safety: registerUserPattern / unregisterUser are NOT ISR-safe and
// should only be invoked from the manager task. Read accessors
// (builtinByIndex, findByKey, forEach) are safe to call concurrently with
// other read accessors but not with mutation. The intended usage is that
// mutation happens before the ISR is armed with a new pattern via apply().

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "PatternRef.h"

namespace PatternLibrary {

// Maximum number of user-registered patterns held simultaneously in M1.2.
// Later milestones (M5 / M6) may bump this; the limit exists to keep the
// registry in static storage (avoids heap fragmentation on Core 1).
static constexpr size_t kUserCapacity = 16;

// Builtin tier — backed by builtin_tables_generated.h.
size_t builtinCount();
const PatternRef* builtinByIndex(size_t i);

// User tier.
bool registerUserPattern(const char* key, PatternRef ref);
bool unregisterUser(const char* key);
size_t userCount();
const PatternRef* userByIndex(size_t i);

// Lookup across builtin -> user -> (future) scratch. Returns nullptr if
// no entry matches. Comparison is byte-wise on the C string key.
const PatternRef* findByKey(const char* key);

// Resolve a legacy Ardu-Stim Wheels[] index (0..pattern_legacy_index_count-1)
// into the builtin PatternRef whose name_key matches the index. Returns
// nullptr if the index is out of range or the migration table references a
// key that is not registered as a builtin.
//
// Consumers (M3.3):
//   * M9.1 legacy serial `L` opcode (Agent E) for the Ardu-Stim Electron GUI.
//   * NVS migration (Agent C) when an old config blob stored a wheel index
//     instead of the new string key.
const PatternRef* findByLegacyIndex(size_t legacy_idx);

// Look up a builtin pattern's human-readable label by its name_key.
// Searches the `pattern_friendly_names` table emitted by
// tools/convert_ardustim_wheels.py (M3.2). Returns nullptr if the key is
// unknown — caller may then fall back to the key itself for display.
const char* friendlyName(const char* name_key);

// Iterate every registered pattern, builtin first then user, invoking
// `fn(ref, tier, user)` for each. `tier` is a static C string literal:
// either "builtin" or "user". Useful for populating the LVGL dropdown.
void forEach(void (*fn)(const PatternRef*, const char* tier, void*), void* user);

} // namespace PatternLibrary
