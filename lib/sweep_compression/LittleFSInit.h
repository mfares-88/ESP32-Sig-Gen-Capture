// lib/sweep_compression/LittleFSInit.h
//
// LittleFS mount + smoke-test helpers, gated on -DSIGGEN_USE_LITTLEFS=1.
// Only the esp32-s3-n4r8 env defines that flag (the WROOM env keeps
// huge_app.csv and ships no filesystem — per §8 risk #1 of
// _Plans-and-Records/implementation_plan.md).
//
// Owner: Agent C (sweep-store). Consumed by Agent E in src/main.cpp.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Mount LittleFS, formatting on first boot if mount fails. Returns true on
// success. On WROOM (no SIGGEN_USE_LITTLEFS flag) this is a no-op that
// returns true so callers don't have to #ifdef every site.
bool initLittleFS();

// Round-trip write + read of `/test.txt` ("siggen-m0.3"). Returns true if
// the readback bytes match. Used by Agent E to satisfy the M0.3 exit
// criterion. On WROOM this returns true (no-op).
bool littleFsSmokeTest();

#ifdef __cplusplus
}
#endif
