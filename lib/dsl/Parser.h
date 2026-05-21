// Parser.h — recursive-descent parser for the runtime pattern DSL.
//
// Parent spec: implementation_plan.md §M5.2 (Agent D, dsl-compiler) +
// integration_report.md §7.1 (BNF grammar).
//
// The parser consumes the Lexer's token stream and emits a ProgramAst
// containing one WheelDef per `:`-separated wheel declaration. Validation
// of value ranges (§7.5) is the Validator's job — the parser only enforces
// grammatical shape and gathers tokens into AST nodes.
//
// Memory:
//   - AST nodes are allocated from regular heap (malloc/new).
//   - The caller must release the returned ProgramAst via freeProgramAst().
//
// Error reporting:
//   - On any parse failure, parse() returns nullptr and populates the
//     caller-supplied err[] buffer + err_off (byte offset into source).
//   - err[] is always zero-terminated when errcap > 0.

#pragma once

#include <stdint.h>
#include <stddef.h>

#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// AST types
// ─────────────────────────────────────────────────────────────────────────────

enum class WheelKind : uint8_t { Symmetric = 0, Missing = 1, Angular = 2 };
enum class Rotation  : uint8_t { CW = 0, CCW = 1 };   // 'C' = CW (360°), 'c' = CCW/cam (720°)

// One <run> entry from a Missing-wheel run-list. value is the count;
// suffix tells us whether it's teeth-present ('t') or teeth-missing ('m').
struct DslRunEntry {
  uint16_t value;
  char     suffix;   // 't' or 'm'
};

// One Angular pair: alternating high/low durations in degrees.
// We store the raw int list as-is; semantic interpretation (alternation
// H/L starting from H) is the Compiler's job.
struct DslAngularEntry {
  uint16_t degrees;
};

struct WheelDef {
  uint8_t   pin;           // 1..4
  Rotation  rotation;      // CW (C, 360°) or CCW (c, 720°)
  WheelKind kind;          // S | M | A
  // Symmetric / Missing fields
  int16_t   duty_num;
  int16_t   duty_den;
  uint16_t  total_teeth;   // S: total teeth; M: total teeth count
  // Missing-only — populated when kind == Missing.
  std::vector<DslRunEntry> runs;   // ordered: e.g. {58t, 2m}
  // Derived sums for convenience (Validator uses these too).
  uint16_t  teeth_with;    // sum of 't' entries
  uint16_t  missing;       // sum of 'm' entries
  // Angular-only — populated when kind == Angular.
  std::vector<DslAngularEntry> angular;

  // Source span for diagnostics (offset of first token, length to last token).
  uint16_t  src_offset;
  uint16_t  src_length;
};

struct ProgramAst {
  std::vector<WheelDef> wheels;
};

// Parse a DSL source string into a ProgramAst.
//   - On success: returns a heap-allocated ProgramAst (caller frees with
//     freeProgramAst()). err[] is set to "" and *err_off to 0 if err_off
//     is non-null.
//   - On failure: returns nullptr, populates err[] (≤ errcap-1 bytes,
//     zero-terminated) and *err_off (byte index into source).
//
// `errcap` must be > 0 when `err` is non-null (caller responsibility).
// `err_off` may be null if the caller doesn't care about the offset.
ProgramAst* parse(const char* source, char* err, size_t errcap, uint16_t* err_off);

// Release a ProgramAst returned by parse(). Safe to call with nullptr.
void freeProgramAst(ProgramAst* ast);
