// Validator.h — enforces the 12 semantic rules from integration_report.md
// §7.5 against a parsed ProgramAst, prior to Compiler invocation.
//
// Parent spec: implementation_plan.md §M5.4.
//
// Rule numbering matches §7.5 exactly. Rules involving the compiled
// buffer size (#9) are *also* re-checked by the Compiler since the LCM
// is computed there; we conservatively pre-check upper bounds where
// cheap (e.g. per-wheel slot count).
//
// Result codes: success returns Validator::OK. On failure the rule index
// and a diagnostic are written into the caller-supplied error sink.

#pragma once

#include <stdint.h>
#include <stddef.h>

struct ProgramAst;

struct ValidationResult {
  bool       ok;
  uint8_t    rule;          // §7.5 rule number that fired (1..12), 0 if ok
  uint16_t   src_offset;    // best-effort byte offset into source
  char       message[96];
};

// `source_len` is the byte length of the DSL source string (for rule #11).
ValidationResult validate(const ProgramAst* ast, size_t source_len);
