// lib/sweep_compression/PatternStorage.cpp
//
// M5.6 implementation. See PatternStorage.h for the file-format contract.
//
// On builds without SIGGEN_USE_LITTLEFS (WROOM, per §8 risk #1) every
// entry point compiles to a no-op that returns false / 0 so callers can
// remain unconditional.

#include "PatternStorage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(SIGGEN_USE_LITTLEFS)

#include <Arduino.h>
#include <LittleFS.h>

#include "esp_heap_caps.h"
#include "mbedtls/sha256.h"

namespace {

constexpr const char* kPatternsDir = "/patterns";
constexpr size_t      kSha256Bytes = 32;

bool buildPath(char* out, size_t outlen, const char* key, const char* ext) {
    if (!key || !*key) return false;
    const size_t klen = strnlen(key, PatternStorage::MAX_KEY_LEN + 1);
    if (klen == 0 || klen > PatternStorage::MAX_KEY_LEN) return false;
    for (size_t i = 0; i < klen; ++i) {
        const char c = key[i];
        // Tight whitelist — alphanumerics, dash, underscore. Anything else
        // could let a malicious key escape /patterns/.
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return false;
    }
    const int n = snprintf(out, outlen, "%s/%s.%s", kPatternsDir, key, ext);
    return n > 0 && (size_t)n < outlen;
}

bool ensurePatternsDir() {
    if (LittleFS.exists(kPatternsDir)) return true;
    return LittleFS.mkdir(kPatternsDir);
}

// Compute SHA-256 of an arbitrary memory range using mbedtls.
bool sha256Range(const uint8_t* data, size_t len, uint8_t out[32]) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    // mbedtls v3 uses mbedtls_sha256_starts (no _ret suffix); ESP-IDF
    // ships v3.x in recent IDF releases. The `0` arg selects SHA-256
    // (vs SHA-224).
    if (mbedtls_sha256_starts(&ctx, 0) != 0) {
        mbedtls_sha256_free(&ctx);
        return false;
    }
    if (mbedtls_sha256_update(&ctx, data, len) != 0) {
        mbedtls_sha256_free(&ctx);
        return false;
    }
    if (mbedtls_sha256_finish(&ctx, out) != 0) {
        mbedtls_sha256_free(&ctx);
        return false;
    }
    mbedtls_sha256_free(&ctx);
    return true;
}

// Compute SHA-256 of a file's contents, streamed (no full buffer needed).
bool sha256OfDslFile(const char* path, uint8_t out[32]) {
    File f = LittleFS.open(path, "r");
    if (!f) return false;

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    if (mbedtls_sha256_starts(&ctx, 0) != 0) {
        mbedtls_sha256_free(&ctx);
        f.close();
        return false;
    }
    uint8_t buf[256];
    while (f.available()) {
        const size_t n = f.read(buf, sizeof(buf));
        if (n == 0) break;
        if (mbedtls_sha256_update(&ctx, buf, n) != 0) {
            mbedtls_sha256_free(&ctx);
            f.close();
            return false;
        }
    }
    if (mbedtls_sha256_finish(&ctx, out) != 0) {
        mbedtls_sha256_free(&ctx);
        f.close();
        return false;
    }
    mbedtls_sha256_free(&ctx);
    f.close();
    return true;
}

}  // namespace

namespace PatternStorage {

bool saveDsl(const char* key, const char* dsl_source) {
    if (!key || !dsl_source) return false;
    if (!ensurePatternsDir())  return false;

    char path[MAX_PATH_LEN];
    if (!buildPath(path, sizeof(path), key, "dsl")) return false;

    File f = LittleFS.open(path, "w");
    if (!f) return false;
    const size_t src_len = strlen(dsl_source);
    const size_t n = f.write(reinterpret_cast<const uint8_t*>(dsl_source),
                             src_len);
    f.close();
    if (n != src_len) return false;

    // Invalidate any stale .bin cache — its hash check would fail on next
    // load anyway, but proactively removing it avoids wasting flash.
    char bin_path[MAX_PATH_LEN];
    if (buildPath(bin_path, sizeof(bin_path), key, "bin")) {
        if (LittleFS.exists(bin_path)) LittleFS.remove(bin_path);
    }
    return true;
}

bool loadDsl(const char* key, char* buf, size_t buflen) {
    if (!buf || buflen == 0) return false;
    char path[MAX_PATH_LEN];
    if (!buildPath(path, sizeof(path), key, "dsl")) return false;
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    const size_t n = f.readBytes(buf, buflen - 1);
    buf[n] = '\0';
    const bool full = (f.available() == 0);
    f.close();
    return full;
}

bool saveCompiledCache(const char*    key,
                       const uint8_t* bytes,
                       uint16_t       len,
                       uint16_t       degrees,
                       float          rpm_scaler) {
    if (!key || !bytes || len == 0) return false;
    if (!ensurePatternsDir())        return false;

    char dsl_path[MAX_PATH_LEN];
    char bin_path[MAX_PATH_LEN];
    if (!buildPath(dsl_path, sizeof(dsl_path), key, "dsl")) return false;
    if (!buildPath(bin_path, sizeof(bin_path), key, "bin")) return false;
    if (!LittleFS.exists(dsl_path)) return false;  // refuse to orphan cache

    uint8_t hash[kSha256Bytes];
    if (!sha256OfDslFile(dsl_path, hash)) return false;

    File f = LittleFS.open(bin_path, "w");
    if (!f) return false;

    bool ok = true;
    ok = ok && (f.write(hash, kSha256Bytes) == kSha256Bytes);
    ok = ok && (f.write(reinterpret_cast<const uint8_t*>(&degrees),
                        sizeof(degrees)) == sizeof(degrees));
    ok = ok && (f.write(reinterpret_cast<const uint8_t*>(&rpm_scaler),
                        sizeof(rpm_scaler)) == sizeof(rpm_scaler));
    ok = ok && (f.write(reinterpret_cast<const uint8_t*>(&len),
                        sizeof(len)) == sizeof(len));
    ok = ok && (f.write(bytes, len) == len);
    f.close();
    if (!ok) {
        LittleFS.remove(bin_path);
        return false;
    }
    return true;
}

bool loadCompiledCache(const char* key,
                       uint8_t**   out_bytes,
                       uint16_t*   out_len,
                       uint16_t*   out_degrees,
                       float*      out_rpm_scaler) {
    if (!out_bytes || !out_len || !out_degrees || !out_rpm_scaler) return false;
    *out_bytes = nullptr;
    *out_len = 0;
    *out_degrees = 0;
    *out_rpm_scaler = 0.0f;

    char dsl_path[MAX_PATH_LEN];
    char bin_path[MAX_PATH_LEN];
    if (!buildPath(dsl_path, sizeof(dsl_path), key, "dsl")) return false;
    if (!buildPath(bin_path, sizeof(bin_path), key, "bin")) return false;

    if (!LittleFS.exists(bin_path)) return false;
    if (!LittleFS.exists(dsl_path)) {
        // Orphan cache — clean up and signal "no cache".
        LittleFS.remove(bin_path);
        return false;
    }

    // SHA-256 invalidation: compare the .bin's leading 32 bytes against
    // a freshly-computed hash of the current .dsl. Mismatch → delete the
    // cache and report failure so the caller recompiles via dslCompile().
    uint8_t expected_hash[kSha256Bytes];
    if (!sha256OfDslFile(dsl_path, expected_hash)) return false;

    File f = LittleFS.open(bin_path, "r");
    if (!f) return false;

    uint8_t stored_hash[kSha256Bytes];
    if (f.read(stored_hash, kSha256Bytes) != kSha256Bytes) { f.close(); return false; }
    if (memcmp(stored_hash, expected_hash, kSha256Bytes) != 0) {
        f.close();
        LittleFS.remove(bin_path);   // invalidate stale cache
        return false;
    }

    uint16_t degrees = 0;
    float    rpm_scaler = 0.0f;
    uint16_t len = 0;
    if (f.read(reinterpret_cast<uint8_t*>(&degrees), sizeof(degrees))
            != sizeof(degrees))    { f.close(); return false; }
    if (f.read(reinterpret_cast<uint8_t*>(&rpm_scaler), sizeof(rpm_scaler))
            != sizeof(rpm_scaler)) { f.close(); return false; }
    if (f.read(reinterpret_cast<uint8_t*>(&len), sizeof(len))
            != sizeof(len))        { f.close(); return false; }
    if (len == 0) { f.close(); return false; }

    // Prefer PSRAM on S3 boards (8MB OPI per platformio.ini); fall back to
    // regular heap when MALLOC_CAP_SPIRAM isn't available.
    uint8_t* buf = static_cast<uint8_t*>(
        heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buf) buf = static_cast<uint8_t*>(malloc(len));
    if (!buf) { f.close(); return false; }

    if (f.read(buf, len) != len) {
        free(buf);
        f.close();
        return false;
    }
    f.close();

    *out_bytes      = buf;
    *out_len        = len;
    *out_degrees    = degrees;
    *out_rpm_scaler = rpm_scaler;
    return true;
}

bool deletePattern(const char* key) {
    char dsl_path[MAX_PATH_LEN];
    char bin_path[MAX_PATH_LEN];
    if (!buildPath(dsl_path, sizeof(dsl_path), key, "dsl")) return false;
    if (!buildPath(bin_path, sizeof(bin_path), key, "bin")) return false;
    bool removed = false;
    if (LittleFS.exists(dsl_path)) removed |= LittleFS.remove(dsl_path);
    if (LittleFS.exists(bin_path)) removed |= LittleFS.remove(bin_path);
    return removed;
}

size_t listPatterns(char keys[][KEY_BUFLEN], size_t max) {
    if (!keys || max == 0) return 0;
    if (!ensurePatternsDir()) return 0;

    File dir = LittleFS.open(kPatternsDir);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return 0;
    }

    size_t count = 0;
    File entry = dir.openNextFile();
    while (entry && count < max) {
        if (!entry.isDirectory()) {
            const char* full = entry.name();
            // full may be either "name.dsl" (LittleFS Arduino quirk) or
            // "/patterns/name.dsl" depending on framework version — handle
            // both by stripping any leading slash + path.
            const char* slash = strrchr(full, '/');
            const char* base  = slash ? slash + 1 : full;
            const size_t blen = strlen(base);
            if (blen > 4 && strcmp(base + blen - 4, ".dsl") == 0) {
                const size_t key_len = blen - 4;
                if (key_len > 0 && key_len <= MAX_KEY_LEN) {
                    memcpy(keys[count], base, key_len);
                    keys[count][key_len] = '\0';
                    ++count;
                }
            }
        }
        entry.close();
        entry = dir.openNextFile();
    }
    if (entry) entry.close();
    dir.close();
    return count;
}

}  // namespace PatternStorage

#else  // !SIGGEN_USE_LITTLEFS — WROOM build, no filesystem.

namespace PatternStorage {

bool   saveDsl(const char*, const char*)                           { return false; }
bool   loadDsl(const char*, char*, size_t)                         { return false; }
bool   saveCompiledCache(const char*, const uint8_t*, uint16_t,
                         uint16_t, float)                          { return false; }
bool   loadCompiledCache(const char*, uint8_t**, uint16_t*,
                         uint16_t*, float*)                        { return false; }
bool   deletePattern(const char*)                                  { return false; }
size_t listPatterns(char[][KEY_BUFLEN], size_t)                    { return 0; }

}  // namespace PatternStorage

#endif  // SIGGEN_USE_LITTLEFS
