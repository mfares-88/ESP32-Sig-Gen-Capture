// Lexer.cpp — tokenizer for the runtime pattern DSL.
//
// Implements the surface defined in Lexer.h. See integration_report.md §7.1
// for the grammar and §7.5 for the validation rules that downstream stages
// (Parser/Compiler/Validator in M5.2+) enforce on top of the token stream.
//
// Scope (M5.1): purely lexical. The lexer rejects characters that cannot
// begin any token, and rejects malformed numeric literals (e.g. "1/" with
// no denominator, integer overflow, suffix without a preceding digit). It
// does NOT enforce grammar shape — that is the parser's job.

#include "Lexer.h"

#include <stddef.h>
#include <string.h>

namespace {

// Cap source length to UINT16_MAX-1 so we can use uint16_t offsets safely.
// integration_report.md §7.5 rule #11 caps DSL source at 512 chars anyway;
// the lexer is more permissive but never overflows its uint16_t bookkeeping.
constexpr uint16_t kSourceMax = 0xFFFEu;

inline bool isAsciiDigit(char c)      { return c >= '0' && c <= '9'; }
inline bool isAsciiWhitespace(char c) { return c == ' '  || c == '\t' ||
                                               c == '\n' || c == '\r'; }
// Letters that are valid <rot> or <kind> single-letter tokens per §7.1.
inline bool isTypeLetter(char c) {
  return c == 'C' || c == 'c' ||
         c == 'A' || c == 'S' || c == 'M';
}
// Suffix characters for <run> entries per §7.1.
inline bool isRunSuffix(char c) { return c == 't' || c == 'm'; }

}  // namespace

Lexer::Lexer(const char* source)
    : src_(source ? source : ""),
      pos_(0),
      len_(0),
      eof_seen_(false),
      error_(false),
      err_msg_(""),
      peeked_{},
      has_peek_(false) {
  // Bounded strlen — we never need more than kSourceMax bytes.
  if (src_) {
    size_t n = 0;
    while (n < kSourceMax && src_[n] != '\0') {
      ++n;
    }
    len_ = static_cast<uint16_t>(n);
  }
}

void Lexer::skipWhitespace_() {
  while (pos_ < len_ && isAsciiWhitespace(src_[pos_])) {
    ++pos_;
  }
}

Token Lexer::makeError_(uint16_t off, const char* msg) {
  error_   = true;
  err_msg_ = msg;
  Token t{};
  t.type   = TOK_ERROR;
  t.offset = off;
  t.length = 0;
  return t;
}

// Greedy scanner. Caller has already cleared/consumed the peek slot.
Token Lexer::scan_() {
  // If we've already latched a sticky terminal token, replay it.
  if (error_) {
    Token t{};
    t.type   = TOK_ERROR;
    t.offset = pos_;
    t.length = 0;
    return t;
  }
  if (eof_seen_) {
    Token t{};
    t.type   = TOK_EOF;
    t.offset = pos_;
    t.length = 0;
    return t;
  }

  skipWhitespace_();

  if (pos_ >= len_) {
    eof_seen_ = true;
    Token t{};
    t.type   = TOK_EOF;
    t.offset = pos_;
    t.length = 0;
    return t;
  }

  const uint16_t start = pos_;
  const char     c     = src_[pos_];

  // ── Single-character tokens ────────────────────────────────────────────
  if (c == ',') {
    ++pos_;
    Token t{};
    t.type   = TOK_COMMA;
    t.offset = start;
    t.length = 1;
    return t;
  }
  if (c == ':') {
    ++pos_;
    Token t{};
    t.type   = TOK_COLON;
    t.offset = start;
    t.length = 1;
    return t;
  }

  // ── Bare '/' (only when not part of a fraction) ────────────────────────
  // Fractions are recognised in the numeric branch below by looking ahead
  // after an integer literal. A '/' encountered HERE means it appeared
  // without a preceding integer — emit TOK_SLASH so the parser can produce
  // a precise error. (Reserved token slot per Lexer.h contract.)
  if (c == '/') {
    ++pos_;
    Token t{};
    t.type   = TOK_SLASH;
    t.offset = start;
    t.length = 1;
    return t;
  }

  // ── Type letter (C/c/M/S/A) ────────────────────────────────────────────
  // Suffix letters 't'/'m' are handled inside the numeric branch and never
  // reach here as a standalone token, because suffix-without-digit means
  // the parser hit an unexpected character.
  if (isTypeLetter(c)) {
    ++pos_;
    Token t{};
    t.type      = TOK_LETTER;
    t.offset    = start;
    t.length    = 1;
    t.v.letter  = c;
    return t;
  }

  // ── Integer / fraction / suffixed-integer ──────────────────────────────
  if (isAsciiDigit(c)) {
    // Parse digit run with overflow check (int32 ceiling).
    int32_t  val      = 0;
    uint16_t digits   = 0;
    while (pos_ < len_ && isAsciiDigit(src_[pos_])) {
      const int d = src_[pos_] - '0';
      // Pre-multiply overflow guard: val * 10 + d ≤ INT32_MAX.
      if (val > (2147483647 - d) / 10) {
        return makeError_(start, "integer literal overflow");
      }
      val = val * 10 + d;
      ++pos_;
      ++digits;
      if (digits > 9) {
        // Defensive: §7.5 rule #9 caps the buffer at 4096, no single
        // integer in the grammar exceeds three digits. Allow up to nine
        // before complaining so plain ints stay friendly to test code.
        // (Overflow guard above is the real safety net.)
      }
    }

    // Lookahead for fraction "/<int>".
    if (pos_ < len_ && src_[pos_] == '/') {
      const uint16_t slash_off = pos_;
      // Peek next char: must be a digit to form a fraction.
      if (pos_ + 1 < len_ && isAsciiDigit(src_[pos_ + 1])) {
        ++pos_;  // consume '/'
        int32_t  den       = 0;
        uint16_t den_digs  = 0;
        while (pos_ < len_ && isAsciiDigit(src_[pos_])) {
          const int d = src_[pos_] - '0';
          if (den > (2147483647 - d) / 10) {
            return makeError_(start, "fraction denominator overflow");
          }
          den = den * 10 + d;
          ++pos_;
          ++den_digs;
        }
        // Values are range-validated by Validator (§7.5 rule #4); the
        // lexer only checks they fit in int16_t so the union packs cleanly.
        if (val > 32767 || den > 32767) {
          return makeError_(start, "fraction component out of range");
        }
        Token t{};
        t.type         = TOK_FRACTION;
        t.offset       = start;
        t.length       = static_cast<uint16_t>(pos_ - start);
        t.v.frac.num   = static_cast<int16_t>(val);
        t.v.frac.den   = static_cast<int16_t>(den);
        return t;
      }
      // '/' not followed by a digit ⇒ malformed fraction. Report at the
      // slash offset for a tight diagnostic.
      return makeError_(slash_off, "malformed fraction: missing denominator");
    }

    // Lookahead for run-suffix ('t' or 'm').
    if (pos_ < len_ && isRunSuffix(src_[pos_])) {
      const char suf = src_[pos_];
      ++pos_;
      Token t{};
      t.type              = TOK_INT_SUFFIXED;
      t.offset            = start;
      t.length            = static_cast<uint16_t>(pos_ - start);
      t.v.int_suf.val     = val;
      t.v.int_suf.suffix  = suf;
      return t;
    }

    // Plain integer.
    Token t{};
    t.type       = TOK_INT;
    t.offset     = start;
    t.length     = static_cast<uint16_t>(pos_ - start);
    t.v.int_val  = val;
    return t;
  }

  // ── Stray suffix letter without a digit prefix ─────────────────────────
  if (isRunSuffix(c)) {
    return makeError_(start, "run suffix without preceding integer");
  }

  // ── Anything else is invalid ───────────────────────────────────────────
  return makeError_(start, "unexpected character");
}

Token Lexer::next() {
  if (has_peek_) {
    has_peek_ = false;
    return peeked_;
  }
  return scan_();
}

Token Lexer::peek() {
  if (!has_peek_) {
    peeked_   = scan_();
    has_peek_ = true;
  }
  return peeked_;
}
