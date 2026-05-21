// lib/sweep_compression/LittleFSInit.cpp
//
// LittleFS bring-up (M0.3). WROOM env compiles this to no-op stubs.

#include "LittleFSInit.h"

#if defined(SIGGEN_USE_LITTLEFS)

#include <Arduino.h>
#include <LittleFS.h>

namespace {
    bool s_mounted = false;
}

bool initLittleFS() {
    if (s_mounted) return true;

    // begin(true) = format-on-fail. First boot after the M0.3 partition swap
    // will hit this path and lay down a fresh filesystem.
    if (!LittleFS.begin(true)) {
        Serial.println("[LittleFS] mount failed even with format-on-fail");
        return false;
    }

    s_mounted = true;
    Serial.printf("[LittleFS] mounted: total=%u used=%u\n",
                  (unsigned)LittleFS.totalBytes(),
                  (unsigned)LittleFS.usedBytes());
    return true;
}

bool littleFsSmokeTest() {
    if (!s_mounted && !initLittleFS()) return false;

    static const char* kPath    = "/test.txt";
    static const char* kPayload = "siggen-m0.3";

    // Write
    {
        File f = LittleFS.open(kPath, "w");
        if (!f) {
            Serial.println("[LittleFS] smoke: open-for-write failed");
            return false;
        }
        const size_t n = f.write(reinterpret_cast<const uint8_t*>(kPayload),
                                 strlen(kPayload));
        f.close();
        if (n != strlen(kPayload)) {
            Serial.println("[LittleFS] smoke: short write");
            return false;
        }
    }

    // Read back
    {
        File f = LittleFS.open(kPath, "r");
        if (!f) {
            Serial.println("[LittleFS] smoke: open-for-read failed");
            return false;
        }
        char buf[32] = {0};
        const size_t n = f.readBytes(buf, sizeof(buf) - 1);
        f.close();
        if (n != strlen(kPayload) || strcmp(buf, kPayload) != 0) {
            Serial.printf("[LittleFS] smoke: mismatch n=%u buf='%s'\n",
                          (unsigned)n, buf);
            return false;
        }
    }

    Serial.println("[LittleFS] smoke test passed");
    return true;
}

#else // !SIGGEN_USE_LITTLEFS  --  WROOM / any env without a LittleFS partition

bool initLittleFS()      { return true; }
bool littleFsSmokeTest() { return true; }

#endif // SIGGEN_USE_LITTLEFS
