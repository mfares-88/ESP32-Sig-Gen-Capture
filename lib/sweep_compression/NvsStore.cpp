// lib/sweep_compression/NvsStore.cpp
//
// M3.5 implementation. See NvsStore.h header for the contract.
//
// Uses the ESP-IDF NVS C API directly (not Arduino `Preferences`) so the
// schema-version handling, partial-write recovery, and migration path are
// explicit. Both the S3 (esp32-s3-n4r8) and WROOM (esp32-wroom32d) envs
// ship NVS in their default partition layout — no build flag required.

#include "NvsStore.h"

#include <string.h>

#include <Arduino.h>            // Serial (logging only)
#include "nvs.h"
#include "nvs_flash.h"

// ---- Canonical globals (single owner) -----------------------------------

char     g_pattern_key[NvsStore::PATTERN_KEY_BUFLEN] = {0};
uint32_t g_rpm              = NvsStore::DEFAULT_RPM;
uint8_t  g_invert_mask      = NvsStore::DEFAULT_INVERT_MASK;

uint16_t g_sweep_low_rpm    = NvsStore::DEFAULT_SWEEP_LOW_RPM;
uint16_t g_sweep_high_rpm   = NvsStore::DEFAULT_SWEEP_HIGH_RPM;
uint8_t  g_sweep_mode       = NvsStore::DEFAULT_SWEEP_MODE;
uint32_t g_sweep_interval_us = NvsStore::DEFAULT_SWEEP_INTERVAL_US;

bool     g_comp_enabled     = NvsStore::DEFAULT_COMP_ENABLED;
uint8_t  g_comp_cyl         = NvsStore::DEFAULT_COMP_CYL;
uint16_t g_comp_rpm_thresh  = NvsStore::DEFAULT_COMP_RPM_THRESH;
uint8_t  g_comp_peak        = NvsStore::DEFAULT_COMP_PEAK;
bool     g_comp_dynamic     = NvsStore::DEFAULT_COMP_DYNAMIC;

namespace {

// ESP-IDF NVS namespace identifier (max 15 chars + NUL).
constexpr const char* kNs = "siggen";

// Key names. Kept short and stable — they're the schema. Adding new keys
// is OK without a SCHEMA_VERSION bump; removing or retyping is not.
constexpr const char* kKeySchemaVersion = "schema_ver";
constexpr const char* kKeyPatternKey    = "pattern_key";
constexpr const char* kKeyRpm           = "rpm";
constexpr const char* kKeyInvertMask    = "invert_mask";

constexpr const char* kKeySweepLow      = "sweep_low";
constexpr const char* kKeySweepHigh     = "sweep_high";
constexpr const char* kKeySweepMode     = "sweep_mode";
constexpr const char* kKeySweepInterval = "sweep_int_us";

constexpr const char* kKeyCompEnabled   = "comp_en";
constexpr const char* kKeyCompCyl       = "comp_cyl";
constexpr const char* kKeyCompRpmThresh = "comp_thresh";
constexpr const char* kKeyCompPeak      = "comp_peak";
constexpr const char* kKeyCompDynamic   = "comp_dyn";

bool s_flash_inited = false;
bool s_ready        = false;

// Open the namespace for read/write. Caller closes via nvs_close().
// Returns true on success.
bool openRw(nvs_handle_t* h) {
    if (!s_ready) return false;
    esp_err_t e = nvs_open(kNs, NVS_READWRITE, h);
    if (e != ESP_OK) {
        Serial.printf("[NVS] nvs_open(siggen, RW) failed: 0x%x\n", (unsigned)e);
        return false;
    }
    return true;
}

bool openRo(nvs_handle_t* h) {
    if (!s_ready) return false;
    esp_err_t e = nvs_open(kNs, NVS_READONLY, h);
    if (e != ESP_OK) {
        // ESP_ERR_NVS_NOT_FOUND is normal on first boot — quietly bail.
        if (e != ESP_ERR_NVS_NOT_FOUND) {
            Serial.printf("[NVS] nvs_open(siggen, RO) failed: 0x%x\n", (unsigned)e);
        }
        return false;
    }
    return true;
}

bool commitAndClose(nvs_handle_t h, const char* tag) {
    esp_err_t e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) {
        Serial.printf("[NVS] commit failed (%s): 0x%x\n", tag, (unsigned)e);
        return false;
    }
    return true;
}

// One-shot u8 / u32 setters used by both the public API and
// saveAllFromGlobals().
bool nvsSetU8(const char* key, uint8_t v) {
    nvs_handle_t h;
    if (!openRw(&h)) return false;
    esp_err_t e = nvs_set_u8(h, key, v);
    if (e != ESP_OK) {
        Serial.printf("[NVS] set_u8(%s) failed: 0x%x\n", key, (unsigned)e);
        nvs_close(h);
        return false;
    }
    return commitAndClose(h, key);
}

bool nvsSetU16(const char* key, uint16_t v) {
    nvs_handle_t h;
    if (!openRw(&h)) return false;
    esp_err_t e = nvs_set_u16(h, key, v);
    if (e != ESP_OK) {
        Serial.printf("[NVS] set_u16(%s) failed: 0x%x\n", key, (unsigned)e);
        nvs_close(h);
        return false;
    }
    return commitAndClose(h, key);
}

bool nvsSetU32(const char* key, uint32_t v) {
    nvs_handle_t h;
    if (!openRw(&h)) return false;
    esp_err_t e = nvs_set_u32(h, key, v);
    if (e != ESP_OK) {
        Serial.printf("[NVS] set_u32(%s) failed: 0x%x\n", key, (unsigned)e);
        nvs_close(h);
        return false;
    }
    return commitAndClose(h, key);
}

// Migration entry-point. v1 is the floor — there's nothing to migrate
// forward FROM yet. Future schemas should pivot here on `from_version`.
void migrate(nvs_handle_t /*h*/, uint8_t from_version) {
    Serial.printf("[NVS] migration stub: from v%u -> v%u (no-op)\n",
                  (unsigned)from_version, (unsigned)NvsStore::SCHEMA_VERSION);
    // Intentionally empty. For v1 -> v2, read v1 keys here and rewrite in
    // the new shape before the caller bumps schema_version.
}

}  // namespace

namespace NvsStore {

bool begin() {
    if (s_ready) return true;

    if (!s_flash_inited) {
        esp_err_t e = nvs_flash_init();
        if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            // Partition is corrupt or was written by a newer IDF. Erase
            // and retry — this is the standard ESP-IDF recovery path and
            // matches our "never block boot" rule.
            Serial.println("[NVS] flash needs erase; reformatting");
            nvs_flash_erase();
            e = nvs_flash_init();
        }
        if (e != ESP_OK) {
            Serial.printf("[NVS] nvs_flash_init failed: 0x%x\n", (unsigned)e);
            return false;
        }
        s_flash_inited = true;
    }

    nvs_handle_t h;
    esp_err_t e = nvs_open(kNs, NVS_READWRITE, &h);
    if (e != ESP_OK) {
        Serial.printf("[NVS] open(siggen) failed: 0x%x\n", (unsigned)e);
        return false;
    }

    // Schema-version check.
    uint8_t found_version = 0;
    e = nvs_get_u8(h, kKeySchemaVersion, &found_version);
    if (e == ESP_ERR_NVS_NOT_FOUND) {
        // First boot in this namespace — stamp the current version and
        // treat as factory init. Defaults already populate globals.
        Serial.printf("[NVS] no schema_version; initializing at v%u\n",
                      (unsigned)SCHEMA_VERSION);
        nvs_set_u8(h, kKeySchemaVersion, SCHEMA_VERSION);
        nvs_commit(h);
    } else if (e == ESP_OK) {
        if (found_version == SCHEMA_VERSION) {
            // happy path
        } else if (found_version < SCHEMA_VERSION) {
            Serial.printf("[NVS] schema v%u found, upgrading to v%u\n",
                          (unsigned)found_version, (unsigned)SCHEMA_VERSION);
            migrate(h, found_version);
            nvs_set_u8(h, kKeySchemaVersion, SCHEMA_VERSION);
            nvs_commit(h);
        } else {
            // Newer schema on flash than this firmware knows. Per §6
            // Agent C hard rule: log + treat as factory; never block.
            Serial.printf("[NVS] schema v%u newer than v%u — using defaults\n",
                          (unsigned)found_version, (unsigned)SCHEMA_VERSION);
        }
    } else {
        Serial.printf("[NVS] get schema_version failed: 0x%x — using defaults\n",
                      (unsigned)e);
    }

    nvs_close(h);
    s_ready = true;
    return true;
}

bool isReady() { return s_ready; }

// ---- pattern_key (string) ----------------------------------------------

bool setPatternKey(const char* key) {
    if (!key) return false;
    nvs_handle_t h;
    if (!openRw(&h)) return false;
    esp_err_t e = nvs_set_str(h, kKeyPatternKey, key);
    if (e != ESP_OK) {
        Serial.printf("[NVS] set_str(pattern_key) failed: 0x%x\n", (unsigned)e);
        nvs_close(h);
        return false;
    }
    return commitAndClose(h, kKeyPatternKey);
}

bool getPatternKey(char* buf, size_t buflen) {
    if (!buf || buflen == 0) return false;
    nvs_handle_t h;
    if (!openRo(&h)) return false;

    size_t required = 0;
    esp_err_t e = nvs_get_str(h, kKeyPatternKey, nullptr, &required);
    if (e != ESP_OK || required == 0 || required > buflen) {
        nvs_close(h);
        return false;
    }
    e = nvs_get_str(h, kKeyPatternKey, buf, &required);
    nvs_close(h);
    return e == ESP_OK;
}

// ---- scalar setters/getters --------------------------------------------

bool setRpm(uint32_t rpm)              { return nvsSetU32(kKeyRpm, rpm); }
bool setInvertMask(uint8_t mask)       { return nvsSetU8(kKeyInvertMask, mask); }

bool getRpm(uint32_t* out) {
    if (!out) return false;
    nvs_handle_t h;
    if (!openRo(&h)) return false;
    esp_err_t e = nvs_get_u32(h, kKeyRpm, out);
    nvs_close(h);
    return e == ESP_OK;
}

bool getInvertMask(uint8_t* out) {
    if (!out) return false;
    nvs_handle_t h;
    if (!openRo(&h)) return false;
    esp_err_t e = nvs_get_u8(h, kKeyInvertMask, out);
    nvs_close(h);
    return e == ESP_OK;
}

// ---- Sweep --------------------------------------------------------------

bool setSweep(uint16_t low_rpm, uint16_t high_rpm, uint8_t mode,
              uint32_t interval_us) {
    nvs_handle_t h;
    if (!openRw(&h)) return false;
    esp_err_t e1 = nvs_set_u16(h, kKeySweepLow,      low_rpm);
    esp_err_t e2 = nvs_set_u16(h, kKeySweepHigh,     high_rpm);
    esp_err_t e3 = nvs_set_u8 (h, kKeySweepMode,     mode);
    esp_err_t e4 = nvs_set_u32(h, kKeySweepInterval, interval_us);
    if (e1 != ESP_OK || e2 != ESP_OK || e3 != ESP_OK || e4 != ESP_OK) {
        Serial.printf("[NVS] setSweep partial fail: 0x%x 0x%x 0x%x 0x%x\n",
                      (unsigned)e1, (unsigned)e2, (unsigned)e3, (unsigned)e4);
        nvs_close(h);
        return false;
    }
    return commitAndClose(h, "sweep_*");
}

bool getSweep(uint16_t* low_rpm, uint16_t* high_rpm, uint8_t* mode,
              uint32_t* interval_us) {
    if (!low_rpm || !high_rpm || !mode || !interval_us) return false;
    nvs_handle_t h;
    if (!openRo(&h)) return false;
    esp_err_t e1 = nvs_get_u16(h, kKeySweepLow,      low_rpm);
    esp_err_t e2 = nvs_get_u16(h, kKeySweepHigh,     high_rpm);
    esp_err_t e3 = nvs_get_u8 (h, kKeySweepMode,     mode);
    esp_err_t e4 = nvs_get_u32(h, kKeySweepInterval, interval_us);
    nvs_close(h);
    return e1 == ESP_OK && e2 == ESP_OK && e3 == ESP_OK && e4 == ESP_OK;
}

// ---- Compression --------------------------------------------------------
//
// Note (M4 forward-compat): the AVR `compressionDynamic < 655U` overflow
// guard from References/ardustim.ino:372 is enforced at runtime by the
// compression task in SweepCompression.cpp — NvsStore just stores the
// `dynamic` bool here. Preserved for parity per implementation_plan.md §8
// risk #5.

bool setCompression(bool enabled, uint8_t cyl, uint16_t rpm_thresh,
                    uint8_t peak, bool dynamic) {
    nvs_handle_t h;
    if (!openRw(&h)) return false;
    esp_err_t e1 = nvs_set_u8 (h, kKeyCompEnabled,   enabled ? 1 : 0);
    esp_err_t e2 = nvs_set_u8 (h, kKeyCompCyl,       cyl);
    esp_err_t e3 = nvs_set_u16(h, kKeyCompRpmThresh, rpm_thresh);
    esp_err_t e4 = nvs_set_u8 (h, kKeyCompPeak,      peak);
    esp_err_t e5 = nvs_set_u8 (h, kKeyCompDynamic,   dynamic ? 1 : 0);
    if (e1 != ESP_OK || e2 != ESP_OK || e3 != ESP_OK || e4 != ESP_OK || e5 != ESP_OK) {
        Serial.printf("[NVS] setCompression partial fail: 0x%x 0x%x 0x%x 0x%x 0x%x\n",
                      (unsigned)e1, (unsigned)e2, (unsigned)e3,
                      (unsigned)e4, (unsigned)e5);
        nvs_close(h);
        return false;
    }
    return commitAndClose(h, "comp_*");
}

bool getCompression(bool* enabled, uint8_t* cyl, uint16_t* rpm_thresh,
                    uint8_t* peak, bool* dynamic) {
    if (!enabled || !cyl || !rpm_thresh || !peak || !dynamic) return false;
    nvs_handle_t h;
    if (!openRo(&h)) return false;
    uint8_t en = 0;
    uint8_t dy = 0;
    esp_err_t e1 = nvs_get_u8 (h, kKeyCompEnabled,   &en);
    esp_err_t e2 = nvs_get_u8 (h, kKeyCompCyl,       cyl);
    esp_err_t e3 = nvs_get_u16(h, kKeyCompRpmThresh, rpm_thresh);
    esp_err_t e4 = nvs_get_u8 (h, kKeyCompPeak,      peak);
    esp_err_t e5 = nvs_get_u8 (h, kKeyCompDynamic,   &dy);
    nvs_close(h);
    if (e1 != ESP_OK || e2 != ESP_OK || e3 != ESP_OK ||
        e4 != ESP_OK || e5 != ESP_OK) {
        return false;
    }
    *enabled = (en != 0);
    *dynamic = (dy != 0);
    return true;
}

// ---- Bulk helpers -------------------------------------------------------

void loadAllToGlobals() {
    // Pattern key — string default if missing.
    char tmp[PATTERN_KEY_BUFLEN] = {0};
    if (getPatternKey(tmp, sizeof(tmp))) {
        strncpy(g_pattern_key, tmp, sizeof(g_pattern_key) - 1);
        g_pattern_key[sizeof(g_pattern_key) - 1] = '\0';
    } else {
        strncpy(g_pattern_key, DEFAULT_PATTERN_KEY, sizeof(g_pattern_key) - 1);
        g_pattern_key[sizeof(g_pattern_key) - 1] = '\0';
    }

    uint32_t rpm_v = 0;
    g_rpm = getRpm(&rpm_v) ? rpm_v : DEFAULT_RPM;

    uint8_t inv = 0;
    g_invert_mask = getInvertMask(&inv) ? inv : DEFAULT_INVERT_MASK;

    uint16_t s_lo = 0, s_hi = 0;
    uint8_t  s_mode = 0;
    uint32_t s_int = 0;
    if (getSweep(&s_lo, &s_hi, &s_mode, &s_int)) {
        g_sweep_low_rpm     = s_lo;
        g_sweep_high_rpm    = s_hi;
        g_sweep_mode        = s_mode;
        g_sweep_interval_us = s_int;
    } else {
        g_sweep_low_rpm     = DEFAULT_SWEEP_LOW_RPM;
        g_sweep_high_rpm    = DEFAULT_SWEEP_HIGH_RPM;
        g_sweep_mode        = DEFAULT_SWEEP_MODE;
        g_sweep_interval_us = DEFAULT_SWEEP_INTERVAL_US;
    }

    bool     c_en = false, c_dyn = false;
    uint8_t  c_cyl = 0, c_peak = 0;
    uint16_t c_th = 0;
    if (getCompression(&c_en, &c_cyl, &c_th, &c_peak, &c_dyn)) {
        g_comp_enabled    = c_en;
        g_comp_cyl        = c_cyl;
        g_comp_rpm_thresh = c_th;
        g_comp_peak       = c_peak;
        g_comp_dynamic    = c_dyn;
    } else {
        g_comp_enabled    = DEFAULT_COMP_ENABLED;
        g_comp_cyl        = DEFAULT_COMP_CYL;
        g_comp_rpm_thresh = DEFAULT_COMP_RPM_THRESH;
        g_comp_peak       = DEFAULT_COMP_PEAK;
        g_comp_dynamic    = DEFAULT_COMP_DYNAMIC;
    }

    Serial.printf("[NVS] loaded: key='%s' rpm=%u inv=0x%02x sweep=(%u,%u,m=%u,%u) "
                  "comp=(en=%d cyl=%u th=%u pk=%u dyn=%d)\n",
                  g_pattern_key, (unsigned)g_rpm, (unsigned)g_invert_mask,
                  (unsigned)g_sweep_low_rpm, (unsigned)g_sweep_high_rpm,
                  (unsigned)g_sweep_mode, (unsigned)g_sweep_interval_us,
                  (int)g_comp_enabled, (unsigned)g_comp_cyl,
                  (unsigned)g_comp_rpm_thresh, (unsigned)g_comp_peak,
                  (int)g_comp_dynamic);
}

void saveAllFromGlobals() {
    setPatternKey(g_pattern_key);
    setRpm(g_rpm);
    setInvertMask(g_invert_mask);
    setSweep(g_sweep_low_rpm, g_sweep_high_rpm, g_sweep_mode,
             g_sweep_interval_us);
    setCompression(g_comp_enabled, g_comp_cyl, g_comp_rpm_thresh,
                   g_comp_peak, g_comp_dynamic);
}

}  // namespace NvsStore
