// lib/sweep_compression/PatternStorage.h
//
// M5.6 (Agent C, sweep-store): LittleFS-backed pattern persistence.
//
// Stores user DSL sources and their compiled byte-table caches under
// `/patterns/<key>.dsl` and `/patterns/<key>.bin`. The compiled cache
// carries a SHA-256 of the source as its leading 32 bytes; on load we
// recompute the hash of the .dsl and reject the cache (returning false,
// forcing the caller to re-compile via Agent D's dslCompile()) when the
// hashes diverge.
//
// File formats:
//   /patterns/<key>.dsl  — UTF-8 DSL source, no header.
//   /patterns/<key>.bin  — packed:
//       offset  size  field
//          0     32   SHA-256 of the .dsl source bytes
//         32      2   uint16 degrees (360 or 720, little-endian)
//         34      4   float  rpm_scaler  (IEEE-754, little-endian)
//         38      2   uint16 byte-table length N
//         40      N   byte table
//
// Build-flag gating: the whole module is a no-op on builds that do not
// define SIGGEN_USE_LITTLEFS (the WROOM env, per §8 risk #1 of
// _Plans-and-Records/implementation_plan.md). The header still declares
// the API surface so consumers can link against it unconditionally.
//
// Owner: Agent C (sweep-store). Consumed by Agent D (DSL compiler) and
// Agent E (UI / serial).

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace PatternStorage {

// Max key length (matches PatternRef.name_key and NvsStore::PATTERN_KEY_BUFLEN).
static constexpr size_t MAX_KEY_LEN  = 63;
static constexpr size_t KEY_BUFLEN   = 64;
static constexpr size_t MAX_PATH_LEN = 96;

// Write a DSL source under `key`. Replaces any existing file. Returns
// true on success. Computes no hash here — the cache is invalidated
// lazily on the next loadCompiledCache() that observes a mismatch.
bool   saveDsl(const char* key, const char* dsl_source);

// Read a DSL source into `buf`. Returns false if the file is missing,
// the buffer is too small, or the FS is unavailable. Always
// null-terminates on success.
bool   loadDsl(const char* key, char* buf, size_t buflen);

// Persist a compiled cache. `bytes`/`len` are the byte table, and
// `degrees`/`rpm_scaler` are the PatternRef metadata needed to
// reconstruct a usable PatternRef on load. The function reads the
// .dsl source associated with `key` to compute the embedded SHA-256;
// if the .dsl is missing the function refuses (returns false) — the
// caller must always saveDsl() first.
bool   saveCompiledCache(const char*    key,
                         const uint8_t* bytes,
                         uint16_t       len,
                         uint16_t       degrees,
                         float          rpm_scaler);

// Load a compiled cache. On success, `*out_bytes` points at a heap
// buffer the caller must `free()` (allocated via malloc on heap;
// PSRAM is preferred when available via heap_caps_malloc). On hash
// mismatch with the .dsl source, the .bin file is deleted and the
// call returns false (forcing the caller to recompile).
bool   loadCompiledCache(const char* key,
                         uint8_t**   out_bytes,
                         uint16_t*   out_len,
                         uint16_t*   out_degrees,
                         float*      out_rpm_scaler);

// Remove both /patterns/<key>.dsl and /patterns/<key>.bin. Missing
// files are not an error. Returns true if at least one file existed
// and was removed, or false if neither existed.
bool   deletePattern(const char* key);

// Enumerate all `.dsl` files under `/patterns/`. Writes up to `max`
// keys (NUL-terminated, ≤ MAX_KEY_LEN chars each) into `keys`, returns
// the number actually written.
size_t listPatterns(char keys[][KEY_BUFLEN], size_t max);

}  // namespace PatternStorage
