// test_dsl_lexer.cpp — unit tests for lib/dsl/Lexer.{h,cpp}.
//
// Scope: M5.1 deliverable for Agent D (dsl-compiler). Verifies that every
// token class declared in Lexer.h is produced for the expected input shape,
// that the canonical example from implementation_plan.md §M5.3 tokenizes
// cleanly, and that five malformed inputs surface TOK_ERROR with sensible
// offsets.
//
// Build modes:
//   1. PlatformIO Unity (`pio test -e <env>`)  — when UNITY_INCLUDE_CONFIG_H
//      is defined or when building under PlatformIO's test runner, this file
//      uses Unity's TEST_ASSERT_* macros and exposes setUp/tearDown/main.
//   2. Native host build  — define DSL_LEXER_TEST_HOST when compiling with
//      g++/clang to run the suite as a normal executable for quick CI runs.
//      This mode uses a tiny built-in assert macro so the file has no
//      external dependencies beyond <stdio.h>/<string.h>.
//
// Either way, the assertions are identical; only the reporting differs.

#include <stdio.h>
#include <string.h>

#include "../lib/dsl/Lexer.h"

// ───────────────────────────────────────────────────────────────────────────
// Lightweight assertion shim. We avoid pulling Unity in the host build so
// developers can compile this file with a single `g++ Lexer.cpp test_...`.
// ───────────────────────────────────────────────────────────────────────────

#if defined(DSL_LEXER_TEST_HOST) || !defined(UNITY_INCLUDE_CONFIG_H)

static int g_failures = 0;
static int g_checks   = 0;

#define DSL_CHECK(cond, msg)                                                  \
  do {                                                                        \
    ++g_checks;                                                               \
    if (!(cond)) {                                                            \
      ++g_failures;                                                           \
      fprintf(stderr, "FAIL %s:%d  %s  (%s)\n", __FILE__, __LINE__, msg, #cond); \
    }                                                                         \
  } while (0)

#define DSL_EQ_INT(actual, expected, msg)                                     \
  do {                                                                        \
    ++g_checks;                                                               \
    long _a = (long)(actual);                                                 \
    long _e = (long)(expected);                                               \
    if (_a != _e) {                                                           \
      ++g_failures;                                                           \
      fprintf(stderr, "FAIL %s:%d  %s  expected=%ld got=%ld\n",               \
              __FILE__, __LINE__, msg, _e, _a);                               \
    }                                                                         \
  } while (0)

#else  // PlatformIO Unity path

#include <unity.h>
#define DSL_CHECK(cond, msg)            TEST_ASSERT_TRUE_MESSAGE((cond), msg)
#define DSL_EQ_INT(actual, exp, msg)    TEST_ASSERT_EQUAL_INT_MESSAGE((exp), (actual), msg)

#endif

// ───────────────────────────────────────────────────────────────────────────
// Test cases
// ───────────────────────────────────────────────────────────────────────────

static void test_empty_source_is_eof(void) {
  Lexer lx("");
  Token t = lx.next();
  DSL_EQ_INT(t.type, TOK_EOF, "empty source ⇒ EOF");
  // EOF is sticky.
  t = lx.next();
  DSL_EQ_INT(t.type, TOK_EOF, "EOF is sticky");
  DSL_CHECK(!lx.hasError(), "no error on empty source");
}

static void test_whitespace_only(void) {
  Lexer lx("   \t \n \r ");
  Token t = lx.next();
  DSL_EQ_INT(t.type, TOK_EOF, "whitespace-only ⇒ EOF");
}

static void test_plain_int(void) {
  Lexer lx("60");
  Token t = lx.next();
  DSL_EQ_INT(t.type, TOK_INT, "TOK_INT");
  DSL_EQ_INT(t.v.int_val, 60, "value");
  DSL_EQ_INT(t.offset, 0, "offset");
  DSL_EQ_INT(t.length, 2, "length");
  DSL_EQ_INT(lx.next().type, TOK_EOF, "trailing EOF");
}

static void test_fraction(void) {
  Lexer lx("1/2");
  Token t = lx.next();
  DSL_EQ_INT(t.type, TOK_FRACTION, "TOK_FRACTION");
  DSL_EQ_INT(t.v.frac.num, 1, "num");
  DSL_EQ_INT(t.v.frac.den, 2, "den");
  DSL_EQ_INT(t.length, 3, "length covers num+slash+den");
  DSL_EQ_INT(lx.next().type, TOK_EOF, "trailing EOF");
}

static void test_int_suffixed_t(void) {
  Lexer lx("58t");
  Token t = lx.next();
  DSL_EQ_INT(t.type, TOK_INT_SUFFIXED, "TOK_INT_SUFFIXED");
  DSL_EQ_INT(t.v.int_suf.val, 58, "value");
  DSL_EQ_INT(t.v.int_suf.suffix, 't', "suffix=t");
  DSL_EQ_INT(t.length, 3, "length");
}

static void test_int_suffixed_m(void) {
  Lexer lx("2m");
  Token t = lx.next();
  DSL_EQ_INT(t.type, TOK_INT_SUFFIXED, "TOK_INT_SUFFIXED");
  DSL_EQ_INT(t.v.int_suf.val, 2, "value");
  DSL_EQ_INT(t.v.int_suf.suffix, 'm', "suffix=m");
}

static void test_letters(void) {
  Lexer lx("C c M S A");
  const char expected[] = {'C', 'c', 'M', 'S', 'A'};
  for (size_t i = 0; i < sizeof(expected); ++i) {
    Token t = lx.next();
    DSL_EQ_INT(t.type, TOK_LETTER, "TOK_LETTER");
    DSL_EQ_INT(t.v.letter, expected[i], "letter matches");
  }
  DSL_EQ_INT(lx.next().type, TOK_EOF, "trailing EOF");
}

static void test_punctuation(void) {
  Lexer lx(",:");
  DSL_EQ_INT(lx.next().type, TOK_COMMA, "comma");
  DSL_EQ_INT(lx.next().type, TOK_COLON, "colon");
  DSL_EQ_INT(lx.next().type, TOK_EOF,   "EOF");
}

static void test_peek_then_next(void) {
  Lexer lx("60,");
  Token p = lx.peek();
  DSL_EQ_INT(p.type, TOK_INT, "peek sees INT");
  Token n = lx.next();
  DSL_EQ_INT(n.type, TOK_INT, "next returns same INT");
  DSL_EQ_INT(n.v.int_val, 60, "value preserved");
  DSL_EQ_INT(lx.next().type, TOK_COMMA, "then comma");
}

// The canonical end-to-end example from implementation_plan.md §M5.3:
//   "1,C,M,1/2,60,58t,2m : 2,c,S,1/2,1"
// expected 24 tokens (excluding EOF):
//   1   ,  C  ,  M  ,  1/2 ,  60 , 58t , 2m  :  2  ,  c  ,  S  ,  1/2  , 1
//   INT , L  , L  , FRAC , INT , INT_SUF, INT_SUF, COLON,
//   INT , L  , L  , FRAC , INT
// with separating COMMAs.
static void test_canonical_example(void) {
  const char* src = "1,C,M,1/2,60,58t,2m : 2,c,S,1/2,1";
  Lexer lx(src);

  struct Step { uint8_t type; const char* tag; };
  Step seq[] = {
      {TOK_INT,          "wA.pin"},
      {TOK_COMMA,        ","},
      {TOK_LETTER,       "wA.rot=C"},
      {TOK_COMMA,        ","},
      {TOK_LETTER,       "wA.kind=M"},
      {TOK_COMMA,        ","},
      {TOK_FRACTION,     "wA.duty=1/2"},
      {TOK_COMMA,        ","},
      {TOK_INT,          "wA.total=60"},
      {TOK_COMMA,        ","},
      {TOK_INT_SUFFIXED, "wA.run=58t"},
      {TOK_COMMA,        ","},
      {TOK_INT_SUFFIXED, "wA.run=2m"},
      {TOK_COLON,        ":"},
      {TOK_INT,          "wB.pin"},
      {TOK_COMMA,        ","},
      {TOK_LETTER,       "wB.rot=c"},
      {TOK_COMMA,        ","},
      {TOK_LETTER,       "wB.kind=S"},
      {TOK_COMMA,        ","},
      {TOK_FRACTION,     "wB.duty=1/2"},
      {TOK_COMMA,        ","},
      {TOK_INT,          "wB.total=1"},
      {TOK_EOF,          "<eof>"},
  };

  for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); ++i) {
    Token t = lx.next();
    DSL_EQ_INT(t.type, seq[i].type, seq[i].tag);
    DSL_CHECK(!lx.hasError(), "no lex error on canonical example");
  }

  // Spot-check semantic values for the interesting tokens.
  // Re-lex (cheap) so we can index by position.
  Lexer lx2(src);
  // Skip 6 tokens (1 , C , M , ) — token at index 6 is the fraction 1/2.
  for (int i = 0; i < 6; ++i) (void)lx2.next();
  Token frac = lx2.next();
  DSL_EQ_INT(frac.type, TOK_FRACTION, "wA fraction");
  DSL_EQ_INT(frac.v.frac.num, 1, "1/2 num");
  DSL_EQ_INT(frac.v.frac.den, 2, "1/2 den");

  // Total-teeth 60.
  (void)lx2.next();              // ','
  Token total = lx2.next();      // 60
  DSL_EQ_INT(total.type, TOK_INT, "wA total INT");
  DSL_EQ_INT(total.v.int_val, 60, "wA total=60");

  // 58t.
  (void)lx2.next();              // ','
  Token run58 = lx2.next();
  DSL_EQ_INT(run58.type, TOK_INT_SUFFIXED, "wA 58t");
  DSL_EQ_INT(run58.v.int_suf.val, 58, "58t value");
  DSL_EQ_INT(run58.v.int_suf.suffix, 't', "58t suffix");
}

// ── Error cases (5 malformed inputs per the dispatch brief) ────────────────

static void test_error_unexpected_char(void) {
  Lexer lx("1,?,2");
  DSL_EQ_INT(lx.next().type, TOK_INT,   "1");
  DSL_EQ_INT(lx.next().type, TOK_COMMA, ",");
  Token bad = lx.next();
  DSL_EQ_INT(bad.type, TOK_ERROR, "TOK_ERROR on '?'");
  DSL_EQ_INT(bad.offset, 2, "error offset at '?'");
  DSL_CHECK(lx.hasError(), "hasError() set");
  DSL_CHECK(strlen(lx.errorMsg()) > 0, "errorMsg non-empty");
  // Error is sticky.
  DSL_EQ_INT(lx.next().type, TOK_ERROR, "error sticky");
}

static void test_error_fraction_missing_denominator(void) {
  Lexer lx("1/");
  Token bad = lx.next();
  DSL_EQ_INT(bad.type, TOK_ERROR, "malformed fraction");
  // Offset should point at the slash (index 1), not the numerator.
  DSL_EQ_INT(bad.offset, 1, "offset at '/'");
}

static void test_error_fraction_followed_by_letter(void) {
  // "1/C" — '/' not followed by a digit ⇒ malformed fraction.
  Lexer lx("1/C");
  Token bad = lx.next();
  DSL_EQ_INT(bad.type, TOK_ERROR, "TOK_ERROR on 1/C");
  DSL_EQ_INT(bad.offset, 1, "offset at '/'");
}

static void test_error_stray_suffix(void) {
  // 't' as the first character has no preceding integer.
  Lexer lx("t");
  Token bad = lx.next();
  DSL_EQ_INT(bad.type, TOK_ERROR, "stray 't' is error");
  DSL_EQ_INT(bad.offset, 0, "offset at 't'");
}

static void test_error_unknown_letter(void) {
  // 'Z' is not in the type-letter set and not a digit/punct — unexpected char.
  Lexer lx("Z");
  Token bad = lx.next();
  DSL_EQ_INT(bad.type, TOK_ERROR, "stray 'Z' is error");
  DSL_EQ_INT(bad.offset, 0, "offset at 'Z'");
}

// Bonus: integer overflow guard.
static void test_error_int_overflow(void) {
  // 11 digits guarantees overflow of int32_t.
  Lexer lx("99999999999");
  Token bad = lx.next();
  DSL_EQ_INT(bad.type, TOK_ERROR, "int overflow detected");
  DSL_EQ_INT(bad.offset, 0, "offset at start of literal");
}

// ───────────────────────────────────────────────────────────────────────────
// Runner
// ───────────────────────────────────────────────────────────────────────────

static void run_all(void) {
  test_empty_source_is_eof();
  test_whitespace_only();
  test_plain_int();
  test_fraction();
  test_int_suffixed_t();
  test_int_suffixed_m();
  test_letters();
  test_punctuation();
  test_peek_then_next();
  test_canonical_example();
  test_error_unexpected_char();
  test_error_fraction_missing_denominator();
  test_error_fraction_followed_by_letter();
  test_error_stray_suffix();
  test_error_unknown_letter();
  test_error_int_overflow();
}

#if defined(DSL_LEXER_TEST_HOST) || !defined(UNITY_INCLUDE_CONFIG_H)

int main(void) {
  run_all();
  if (g_failures == 0) {
    fprintf(stdout, "[OK] DSL lexer: %d checks passed\n", g_checks);
    return 0;
  }
  fprintf(stdout, "[FAIL] DSL lexer: %d/%d checks failed\n",
          g_failures, g_checks);
  return 1;
}

#else  // Unity path

void setUp(void)    {}
void tearDown(void) {}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_empty_source_is_eof);
  RUN_TEST(test_whitespace_only);
  RUN_TEST(test_plain_int);
  RUN_TEST(test_fraction);
  RUN_TEST(test_int_suffixed_t);
  RUN_TEST(test_int_suffixed_m);
  RUN_TEST(test_letters);
  RUN_TEST(test_punctuation);
  RUN_TEST(test_peek_then_next);
  RUN_TEST(test_canonical_example);
  RUN_TEST(test_error_unexpected_char);
  RUN_TEST(test_error_fraction_missing_denominator);
  RUN_TEST(test_error_fraction_followed_by_letter);
  RUN_TEST(test_error_stray_suffix);
  RUN_TEST(test_error_unknown_letter);
  RUN_TEST(test_error_int_overflow);
  return UNITY_END();
}

#endif
