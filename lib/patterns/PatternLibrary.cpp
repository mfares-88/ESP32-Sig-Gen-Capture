// PatternLibrary.cpp — see PatternLibrary.h for the contract.
//
// M1.2 implementation notes:
//   * Builtin tier is a thin wrapper over the static array in
//     builtin_tables_generated.h. No copy, no init-time work.
//   * User tier uses a fixed-size POD array (no std::vector) to keep
//     allocation patterns predictable and to avoid pulling the STL
//     allocator into ISR-adjacent translation units.
//   * Scratch tier is not implemented in M1.2 — findByKey() falls
//     through to nullptr.

#include "PatternLibrary.h"

#include "builtin_tables_generated.h"
#include "pattern_legacy_index_generated.h"
#include "pattern_names_generated.h"

#include <string.h>

namespace {

struct UserSlot {
    bool        used;
    const char* key;   // NOT owned; caller guarantees lifetime
    PatternRef  ref;   // table/name_key inside also NOT owned
};

UserSlot s_user[PatternLibrary::kUserCapacity];

// Tier label literals — pointer identity matters; forEach() exposes them.
static const char kTierBuiltin[] = "builtin";
static const char kTierUser[]    = "user";

bool key_equal(const char* a, const char* b) {
    if (a == b) return true;
    if (a == nullptr || b == nullptr) return false;
    return strcmp(a, b) == 0;
}

} // namespace

namespace PatternLibrary {

size_t builtinCount() {
    return builtin_pattern_count;
}

const PatternRef* builtinByIndex(size_t i) {
    if (i >= builtin_pattern_count) return nullptr;
    return &builtin_patterns[i];
}

bool registerUserPattern(const char* key, PatternRef ref) {
    if (key == nullptr || *key == '\0') return false;

    // Reject duplicates — caller must explicitly unregister first.
    for (size_t i = 0; i < kUserCapacity; ++i) {
        if (s_user[i].used && key_equal(s_user[i].key, key)) {
            return false;
        }
    }
    // Find first free slot.
    for (size_t i = 0; i < kUserCapacity; ++i) {
        if (!s_user[i].used) {
            s_user[i].used = true;
            s_user[i].key  = key;
            s_user[i].ref  = ref;
            return true;
        }
    }
    return false; // capacity exhausted
}

bool unregisterUser(const char* key) {
    if (key == nullptr) return false;
    for (size_t i = 0; i < kUserCapacity; ++i) {
        if (s_user[i].used && key_equal(s_user[i].key, key)) {
            s_user[i].used = false;
            s_user[i].key  = nullptr;
            s_user[i].ref  = PatternRef{};
            return true;
        }
    }
    return false;
}

size_t userCount() {
    size_t n = 0;
    for (size_t i = 0; i < kUserCapacity; ++i) {
        if (s_user[i].used) ++n;
    }
    return n;
}

const PatternRef* userByIndex(size_t i) {
    // Linear index over the *used* subset, in slot order.
    size_t seen = 0;
    for (size_t s = 0; s < kUserCapacity; ++s) {
        if (!s_user[s].used) continue;
        if (seen == i) return &s_user[s].ref;
        ++seen;
    }
    return nullptr;
}

const PatternRef* findByKey(const char* key) {
    if (key == nullptr) return nullptr;

    // Tier 1: builtin
    for (size_t i = 0; i < builtin_pattern_count; ++i) {
        if (key_equal(builtin_patterns[i].name_key, key)) {
            return &builtin_patterns[i];
        }
    }
    // Tier 2: user
    for (size_t i = 0; i < kUserCapacity; ++i) {
        if (s_user[i].used && key_equal(s_user[i].key, key)) {
            return &s_user[i].ref;
        }
    }
    // Tier 3: scratch — not implemented in M1.2.
    return nullptr;
}

const PatternRef* findByLegacyIndex(size_t legacy_idx) {
    if (legacy_idx >= pattern_legacy_index_count) return nullptr;
    const char* key = pattern_legacy_index_to_key[legacy_idx];
    if (key == nullptr) return nullptr;
    // Builtin tier only — the legacy index space is by definition the
    // Ardu-Stim Wheels[] order, all of which live in builtin_patterns[].
    for (size_t i = 0; i < builtin_pattern_count; ++i) {
        if (key_equal(builtin_patterns[i].name_key, key)) {
            return &builtin_patterns[i];
        }
    }
    return nullptr;
}

const char* friendlyName(const char* name_key) {
    if (name_key == nullptr) return nullptr;
    for (size_t i = 0; i < pattern_friendly_name_count; ++i) {
        if (key_equal(pattern_friendly_names[i].key, name_key)) {
            return pattern_friendly_names[i].friendly;
        }
    }
    return nullptr;
}

void forEach(void (*fn)(const PatternRef*, const char* tier, void*), void* user) {
    if (fn == nullptr) return;
    for (size_t i = 0; i < builtin_pattern_count; ++i) {
        fn(&builtin_patterns[i], kTierBuiltin, user);
    }
    for (size_t i = 0; i < kUserCapacity; ++i) {
        if (s_user[i].used) {
            fn(&s_user[i].ref, kTierUser, user);
        }
    }
}

} // namespace PatternLibrary
