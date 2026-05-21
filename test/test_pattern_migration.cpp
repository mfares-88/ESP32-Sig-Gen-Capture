// test_pattern_migration.cpp — unit tests for the M3.3 legacy-index
// migration table and the M3.2 friendly-name lookup, both exposed via
// lib/patterns/PatternLibrary.{h,cpp}.
//
// Scope (M3.3):
//   * Legacy index 0 must resolve to "dizzy_four_cylinder" — the first row
//     of References/ardustim.ino:59-125 (Wheels[]).
//   * Legacy index for `sixty_minus_two` must round-trip via the migration
//     table: pattern_legacy_index_to_key[3] == "sixty_minus_two" (row 4 of
//     the Wheels[] initializer, zero-indexed).
//   * Out-of-range index must return nullptr.
//   * friendlyName() smoke check: known key returns non-null; unknown key
//     returns nullptr; nullptr input is tolerated.
//
// Build modes:
//   1. PlatformIO Unity (`pio test -e <env> -f test_pattern_migration`)
//      — uses Unity TEST_ASSERT_* macros.
//   2. Native host build  — define PATTERN_MIGRATION_TEST_HOST when
//      compiling with g++/clang to run as a standalone executable. The
//      generated headers depend only on <stddef.h>/<stdint.h> and the
//      PatternRef.h struct, so they compile cleanly off-target.
//
// Either path exercises the same assertions; only the reporting differs.

#include <stdio.h>
#include <string.h>

#include "../lib/patterns/PatternLibrary.h"

// ───────────────────────────────────────────────────────────────────────────
// Lightweight assertion shim (same shape as test_dsl_lexer.cpp).
// ───────────────────────────────────────────────────────────────────────────
#if defined(PATTERN_MIGRATION_TEST_HOST) || !defined(UNITY_INCLUDE_CONFIG_H)

static int g_failures = 0;
static int g_checks   = 0;

#define PM_CHECK(cond, msg)                                                   \
  do {                                                                        \
    ++g_checks;                                                               \
    if (!(cond)) {                                                            \
      ++g_failures;                                                           \
      fprintf(stderr, "FAIL %s:%d  %s  (%s)\n", __FILE__, __LINE__, msg, #cond); \
    }                                                                         \
  } while (0)

#define PM_EQ_STR(actual, expected, msg)                                      \
  do {                                                                        \
    ++g_checks;                                                               \
    const char* _a = (actual);                                                \
    const char* _e = (expected);                                              \
    if (_a == nullptr || _e == nullptr || strcmp(_a, _e) != 0) {              \
      ++g_failures;                                                           \
      fprintf(stderr, "FAIL %s:%d  %s  expected=%s got=%s\n",                 \
              __FILE__, __LINE__, msg,                                        \
              _e ? _e : "(null)", _a ? _a : "(null)");                        \
    }                                                                         \
  } while (0)

#else  // PlatformIO Unity path

#include <unity.h>
#define PM_CHECK(cond, msg)             TEST_ASSERT_TRUE_MESSAGE((cond), msg)
#define PM_EQ_STR(actual, exp, msg)     TEST_ASSERT_EQUAL_STRING_MESSAGE((exp), (actual), msg)

#endif

// ───────────────────────────────────────────────────────────────────────────
// Test cases
// ───────────────────────────────────────────────────────────────────────────

// M3.3-T1: legacy index 0 -> "dizzy_four_cylinder".
// Verified against References/ardustim.ino:61 (first Wheels[] row).
static void test_legacy_index_zero() {
    const PatternRef* p = PatternLibrary::findByLegacyIndex(0);
    PM_CHECK(p != nullptr, "legacy index 0 must resolve to a builtin PatternRef");
    if (p != nullptr) {
        PM_EQ_STR(p->name_key, "dizzy_four_cylinder",
                  "legacy index 0 must be dizzy_four_cylinder per ardustim.ino:61");
    }
}

// M3.3-T2: legacy index for sixty_minus_two — that is row 4 (index 3) of
// Wheels[] per References/ardustim.ino:64. Verifies both directions:
// table -> name_key, and findByKey(name_key) -> same PatternRef.
static void test_legacy_index_sixty_minus_two() {
    const PatternRef* p = PatternLibrary::findByLegacyIndex(3);
    PM_CHECK(p != nullptr, "legacy index 3 must resolve to a builtin PatternRef");
    if (p != nullptr) {
        PM_EQ_STR(p->name_key, "sixty_minus_two",
                  "legacy index 3 must be sixty_minus_two per ardustim.ino:64");

        // Round-trip: findByKey("sixty_minus_two") must yield the same row.
        const PatternRef* by_key = PatternLibrary::findByKey("sixty_minus_two");
        PM_CHECK(by_key == p,
                 "findByKey(\"sixty_minus_two\") must equal findByLegacyIndex(3)");

        // M2.2 sanity: slot_count and rpm_scaler match the spec.
        PM_CHECK(p->slot_count == 120, "sixty_minus_two slot_count must be 120");
        PM_CHECK(p->channel_mask == 0x01,
                 "sixty_minus_two is crank-only -> channel_mask = 0x01");
    }
}

// M3.3-T3: out-of-range index must return nullptr (no UB, no aborts).
static void test_legacy_index_out_of_range() {
    PM_CHECK(PatternLibrary::findByLegacyIndex(64) == nullptr,
             "index == count must be nullptr");
    PM_CHECK(PatternLibrary::findByLegacyIndex(1000) == nullptr,
             "very large index must be nullptr");
    // size_t is unsigned — (size_t)-1 is the max value and also out of range.
    PM_CHECK(PatternLibrary::findByLegacyIndex(static_cast<size_t>(-1)) == nullptr,
             "max size_t must be nullptr");
}

// M3.2 spot check: friendlyName() smoke test against the generated table.
static void test_friendly_name_lookup() {
    const char* label = PatternLibrary::friendlyName("sixty_minus_two");
    PM_CHECK(label != nullptr, "friendlyName(sixty_minus_two) must be non-null");
    if (label != nullptr) {
        PM_EQ_STR(label, "60-2 crank only",
                  "label must match wheel_defs.h sixty_minus_two_friendly_name");
    }

    PM_CHECK(PatternLibrary::friendlyName("no_such_key_xyz") == nullptr,
             "unknown key must return nullptr");
    PM_CHECK(PatternLibrary::friendlyName(nullptr) == nullptr,
             "nullptr key must be tolerated and return nullptr");
}

// M2.2 sanity: with-cam variants are present and report 2-channel mask.
static void test_with_cam_channel_masks() {
    const PatternRef* p1 = PatternLibrary::findByKey("sixty_minus_two_with_cam");
    PM_CHECK(p1 != nullptr, "sixty_minus_two_with_cam must be a builtin");
    if (p1 != nullptr) {
        PM_CHECK(p1->channel_mask == 0x03,
                 "with-cam variant must report channel_mask 0x03 (crank+cam1)");
        PM_CHECK(p1->slot_count == 240, "with-cam variant has 240 edges (720 deg)");
    }

    const PatternRef* p2 = PatternLibrary::findByKey("thirty_six_minus_one_with_cam_fe3");
    PM_CHECK(p2 != nullptr, "thirty_six_minus_one_with_cam_fe3 must be a builtin");
    if (p2 != nullptr) {
        PM_CHECK(p2->channel_mask == 0x03,
                 "FE3 36-1 with cam must report channel_mask 0x03");
    }
}

// ───────────────────────────────────────────────────────────────────────────
// Drivers
// ───────────────────────────────────────────────────────────────────────────

#if defined(PATTERN_MIGRATION_TEST_HOST) || !defined(UNITY_INCLUDE_CONFIG_H)

int main() {
    test_legacy_index_zero();
    test_legacy_index_sixty_minus_two();
    test_legacy_index_out_of_range();
    test_friendly_name_lookup();
    test_with_cam_channel_masks();

    printf("pattern-migration: %d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

#else  // Unity

void setUp() {}
void tearDown() {}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_legacy_index_zero);
    RUN_TEST(test_legacy_index_sixty_minus_two);
    RUN_TEST(test_legacy_index_out_of_range);
    RUN_TEST(test_friendly_name_lookup);
    RUN_TEST(test_with_cam_channel_masks);
    return UNITY_END();
}

#endif
