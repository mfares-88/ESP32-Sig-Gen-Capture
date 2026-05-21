// Lexer.h — tokenizer for the runtime pattern DSL.
//
// Parent spec: implementation_plan.md §M5.1 (Agent D, dsl-compiler) +
// integration_report.md §7.1 (BNF grammar).
//
// Grammar tokens covered:
//
//   <pin>        := one decimal digit '1'..'4'         → TOK_INT (value 1..4)
//   <rot>/<kind> := single letter 'C'|'c'|'A'|'S'|'M'  → TOK_LETTER
//   <duty>       := <int> '/' <int>                    → TOK_FRACTION (one token)
//   <run>        := <int> ('t'|'m')                    → TOK_INT_SUFFIXED
//   integers     := one or more decimal digits         → TOK_INT
//   separators   := ',' ':'                            → TOK_COMMA / TOK_COLON
//   bare '/'     := only when not part of a fraction   → TOK_SLASH (reserved)
//
// Whitespace (space, tab, CR, LF) between tokens is skipped silently.
// On invalid input the lexer returns TOK_ERROR with `offset` set to the
// problem character; subsequent next()/peek() calls keep returning TOK_ERROR
// (sticky error state) until the lexer is destroyed.
//
// Disambiguation contract (mandate for downstream consumers in M5.2+):
//   - "1/2"      → single TOK_FRACTION  (num=1, den=2)
//   - "60"       → single TOK_INT       (60)
//   - "58t"      → single TOK_INT_SUFFIXED (val=58, suffix='t')
//   - "C", "M", "c", "S", "A" → single TOK_LETTER (.letter = that char)
//
// The token boundary is greedy on digits: as soon as the digit run ends, the
// lexer peeks the next byte to decide between TOK_FRACTION (next is '/' and a
// digit follows), TOK_INT_SUFFIXED (next is 't' or 'm'), or plain TOK_INT.
//
// EOF is sticky: once produced, every subsequent next()/peek() returns TOK_EOF
// (unless an error fires first).

#pragma once

#include <stdint.h>

// Token type codes. uint8_t-sized to keep Token compact.
enum DslTokenType : uint8_t {
  TOK_EOF          = 0,
  TOK_INT          = 1,  // plain integer (e.g., "60", "1", "120")
  TOK_FRACTION     = 2,  // duty fraction "n/d"
  TOK_INT_SUFFIXED = 3,  // run-list entry "<int>('t'|'m')"
  TOK_LETTER       = 4,  // C/c/M/S/A type code
  TOK_COMMA        = 5,  // ','
  TOK_COLON        = 6,  // ':'
  TOK_SLASH        = 7,  // bare '/' — only emitted if not part of TOK_FRACTION
  TOK_ERROR        = 255 // malformed input; consult errorMsg() and .offset
};

struct Token {
  uint8_t  type;     // DslTokenType
  uint16_t offset;   // byte offset into source where this token starts
  uint16_t length;   // length in bytes (0 for EOF, 1+ otherwise)
  union Value {
    int32_t int_val;                                  // TOK_INT
    struct { int16_t num; int16_t den; } frac;        // TOK_FRACTION
    struct { int32_t val;  char    suffix; } int_suf; // TOK_INT_SUFFIXED
    char    letter;                                   // TOK_LETTER
  } v;
};

class Lexer {
 public:
  // Source pointer must outlive the Lexer; the lexer does not copy.
  // A null pointer is treated as an empty source ⇒ immediate TOK_EOF.
  explicit Lexer(const char* source);

  // Returns the next token and advances. After TOK_EOF or TOK_ERROR is
  // produced once, all subsequent calls return that same sticky token.
  Token next();

  // Returns the next token without advancing. Idempotent.
  Token peek();

  // Current scan offset into the source string.
  uint16_t offset() const { return pos_; }

  // True after a TOK_ERROR has been produced.
  bool hasError() const { return error_; }

  // Brief diagnostic populated when hasError() is true. Stable empty
  // string ("") in the non-error state. Never returns nullptr.
  const char* errorMsg() const { return err_msg_; }

 private:
  // Core scanner; produces the next token from current position.
  Token scan_();

  // Helpers.
  void  skipWhitespace_();
  Token makeError_(uint16_t off, const char* msg);

  const char* src_;
  uint16_t    pos_;       // current scan offset
  uint16_t    len_;       // total source length (clamped to UINT16_MAX-1)
  bool        eof_seen_;  // sticky EOF flag
  bool        error_;     // sticky error flag
  const char* err_msg_;
  Token       peeked_;
  bool        has_peek_;
};
