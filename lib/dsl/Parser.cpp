// Parser.cpp — recursive-descent parser for the runtime pattern DSL.
//
// Grammar (integration_report.md §7.1):
//
//   <pattern-group>   ::= <wheel-def> { ":" <wheel-def> }
//   <wheel-def>       ::= <pin> "," <rot> "," <kind> "," <kind-tail>
//   <pin>             ::= "1" | "2" | "3" | "4"
//   <rot>             ::= "C" | "c"
//   <kind>            ::= "A" | "S" | "M"
//   <kind-tail>       ::= <sym-tail> | <miss-tail> | <ang-tail>
//   <sym-tail>        ::= <duty> "," <total-teeth>
//   <miss-tail>       ::= <duty> "," <total-teeth> "," <run-list>
//   <ang-tail>        ::= <int> { "," <int> }
//   <duty>            ::= <int> "/" <int>
//   <run-list>        ::= <run> { "," <run> }
//   <run>             ::= <int> ("t" | "m")
//
// The parser is intentionally permissive on value ranges — the Validator
// (M5.4) enforces §7.5 rules. We only catch *grammatical* errors here:
// missing tokens, wrong token types at fixed positions, EOF mid-rule.

#include "Parser.h"
#include "Lexer.h"

#include <stdio.h>
#include <string.h>

namespace {

// Copy a diagnostic into the caller's err buffer, set the offset, and
// return nullptr from the calling site. Always leaves err[] terminated.
ProgramAst* fail(ProgramAst* partial,
                 const char* msg, uint16_t off,
                 char* err, size_t errcap, uint16_t* err_off) {
  if (err && errcap > 0) {
    // Bounded copy with terminator.
    size_t n = strlen(msg);
    if (n >= errcap) n = errcap - 1;
    memcpy(err, msg, n);
    err[n] = '\0';
  }
  if (err_off) *err_off = off;
  if (partial) freeProgramAst(partial);
  return nullptr;
}

// Format a "expected X got <token>" message into a small static buffer.
// The message is then copied by fail() so the buffer can be reused
// across calls.
const char* expectedMsg(const char* what, const Token& got) {
  static char buf[80];
  const char* tok_name = "?";
  switch (got.type) {
    case TOK_EOF:          tok_name = "<eof>"; break;
    case TOK_INT:          tok_name = "integer"; break;
    case TOK_FRACTION:     tok_name = "fraction"; break;
    case TOK_INT_SUFFIXED: tok_name = "suffixed integer"; break;
    case TOK_LETTER:       tok_name = "letter"; break;
    case TOK_COMMA:        tok_name = "','"; break;
    case TOK_COLON:        tok_name = "':'"; break;
    case TOK_SLASH:        tok_name = "'/'"; break;
    case TOK_ERROR:        tok_name = "<lex-error>"; break;
    default: break;
  }
  // Truncate via snprintf; buf is small but the result fits.
  snprintf(buf, sizeof(buf), "expected %s, got %s", what, tok_name);
  return buf;
}

// Parse one <wheel-def>. Returns true on success and appends the wheel
// to ast->wheels; returns false and populates err on failure.
bool parseWheel(Lexer& lx, ProgramAst* ast,
                char* err, size_t errcap, uint16_t* err_off) {
  WheelDef w{};
  w.duty_num = 0;
  w.duty_den = 0;
  w.total_teeth = 0;
  w.teeth_with = 0;
  w.missing = 0;
  w.src_offset = lx.peek().offset;
  w.src_length = 0;

  // ── <pin> ─────────────────────────────────────────────────────────────
  Token t = lx.next();
  if (t.type != TOK_INT) {
    (void)fail(nullptr, expectedMsg("pin", t), t.offset, err, errcap, err_off);
    return false;
  }
  // Pin range check is the Validator's job; we just narrow the type.
  if (t.v.int_val < 0 || t.v.int_val > 255) {
    (void)fail(nullptr, "pin out of range", t.offset, err, errcap, err_off);
    return false;
  }
  w.pin = static_cast<uint8_t>(t.v.int_val);

  // ── "," ───────────────────────────────────────────────────────────────
  t = lx.next();
  if (t.type != TOK_COMMA) {
    (void)fail(nullptr, expectedMsg("',' after pin", t), t.offset, err, errcap, err_off);
    return false;
  }

  // ── <rot> ─────────────────────────────────────────────────────────────
  t = lx.next();
  if (t.type != TOK_LETTER) {
    (void)fail(nullptr, expectedMsg("rotation letter (C/c)", t), t.offset, err, errcap, err_off);
    return false;
  }
  if (t.v.letter == 'C') {
    w.rotation = Rotation::CW;
  } else if (t.v.letter == 'c') {
    w.rotation = Rotation::CCW;
  } else {
    (void)fail(nullptr, "rotation must be 'C' or 'c'", t.offset, err, errcap, err_off);
    return false;
  }

  // ── "," ───────────────────────────────────────────────────────────────
  t = lx.next();
  if (t.type != TOK_COMMA) {
    (void)fail(nullptr, expectedMsg("',' after rotation", t), t.offset, err, errcap, err_off);
    return false;
  }

  // ── <kind> ────────────────────────────────────────────────────────────
  t = lx.next();
  if (t.type != TOK_LETTER) {
    (void)fail(nullptr, expectedMsg("kind letter (S/M/A)", t), t.offset, err, errcap, err_off);
    return false;
  }
  switch (t.v.letter) {
    case 'S': w.kind = WheelKind::Symmetric; break;
    case 'M': w.kind = WheelKind::Missing;   break;
    case 'A': w.kind = WheelKind::Angular;   break;
    default:
      (void)fail(nullptr, "kind must be 'S', 'M', or 'A'", t.offset, err, errcap, err_off);
      return false;
  }

  // ── "," ───────────────────────────────────────────────────────────────
  t = lx.next();
  if (t.type != TOK_COMMA) {
    (void)fail(nullptr, expectedMsg("',' after kind", t), t.offset, err, errcap, err_off);
    return false;
  }

  // ── <kind-tail> ───────────────────────────────────────────────────────
  if (w.kind == WheelKind::Symmetric) {
    // <duty> "," <total-teeth>
    t = lx.next();
    if (t.type != TOK_FRACTION) {
      (void)fail(nullptr, expectedMsg("duty fraction", t), t.offset, err, errcap, err_off);
      return false;
    }
    w.duty_num = t.v.frac.num;
    w.duty_den = t.v.frac.den;

    t = lx.next();
    if (t.type != TOK_COMMA) {
      (void)fail(nullptr, expectedMsg("',' after duty", t), t.offset, err, errcap, err_off);
      return false;
    }
    t = lx.next();
    if (t.type != TOK_INT) {
      (void)fail(nullptr, expectedMsg("total teeth", t), t.offset, err, errcap, err_off);
      return false;
    }
    if (t.v.int_val < 0 || t.v.int_val > 0xFFFF) {
      (void)fail(nullptr, "total teeth out of range", t.offset, err, errcap, err_off);
      return false;
    }
    w.total_teeth = static_cast<uint16_t>(t.v.int_val);

  } else if (w.kind == WheelKind::Missing) {
    // <duty> "," <total-teeth> "," <run-list>
    t = lx.next();
    if (t.type != TOK_FRACTION) {
      (void)fail(nullptr, expectedMsg("duty fraction", t), t.offset, err, errcap, err_off);
      return false;
    }
    w.duty_num = t.v.frac.num;
    w.duty_den = t.v.frac.den;

    t = lx.next();
    if (t.type != TOK_COMMA) {
      (void)fail(nullptr, expectedMsg("',' after duty", t), t.offset, err, errcap, err_off);
      return false;
    }
    t = lx.next();
    if (t.type != TOK_INT) {
      (void)fail(nullptr, expectedMsg("total teeth", t), t.offset, err, errcap, err_off);
      return false;
    }
    if (t.v.int_val < 0 || t.v.int_val > 0xFFFF) {
      (void)fail(nullptr, "total teeth out of range", t.offset, err, errcap, err_off);
      return false;
    }
    w.total_teeth = static_cast<uint16_t>(t.v.int_val);

    t = lx.next();
    if (t.type != TOK_COMMA) {
      (void)fail(nullptr, expectedMsg("',' after total teeth", t), t.offset, err, errcap, err_off);
      return false;
    }

    // <run-list> ::= <run> { "," <run> }
    // <run> ::= TOK_INT_SUFFIXED (val + 't'|'m')
    for (;;) {
      t = lx.next();
      if (t.type != TOK_INT_SUFFIXED) {
        (void)fail(nullptr, expectedMsg("run entry (Nt or Nm)", t), t.offset, err, errcap, err_off);
        return false;
      }
      if (t.v.int_suf.val < 0 || t.v.int_suf.val > 0xFFFF) {
        (void)fail(nullptr, "run entry out of range", t.offset, err, errcap, err_off);
        return false;
      }
      DslRunEntry e{};
      e.value  = static_cast<uint16_t>(t.v.int_suf.val);
      e.suffix = t.v.int_suf.suffix;
      w.runs.push_back(e);
      if (e.suffix == 't') w.teeth_with += e.value;
      else                 w.missing    += e.value;

      // Lookahead for continuation comma vs. wheel boundary / EOF.
      Token p = lx.peek();
      if (p.type == TOK_COMMA) {
        // Could be the start of another <run> — peek further by consuming
        // and re-peeking. We consume here and loop.
        (void)lx.next();
        // After a continuation comma a TOK_INT_SUFFIXED must follow (the
        // run-list cannot end on a trailing comma). The loop handles this.
        continue;
      }
      break;  // run-list terminated by ':' / TOK_EOF / TOK_ERROR
    }

  } else {
    // Angular: <int> { "," <int> }
    for (;;) {
      t = lx.next();
      if (t.type != TOK_INT) {
        (void)fail(nullptr, expectedMsg("angular degree integer", t), t.offset, err, errcap, err_off);
        return false;
      }
      if (t.v.int_val < 0 || t.v.int_val > 0xFFFF) {
        (void)fail(nullptr, "angular value out of range", t.offset, err, errcap, err_off);
        return false;
      }
      DslAngularEntry e{};
      e.degrees = static_cast<uint16_t>(t.v.int_val);
      w.angular.push_back(e);

      Token p = lx.peek();
      if (p.type == TOK_COMMA) {
        (void)lx.next();
        continue;
      }
      break;
    }
  }

  // Capture trailing span.
  w.src_length = static_cast<uint16_t>(lx.offset() - w.src_offset);
  ast->wheels.push_back(std::move(w));
  return true;
}

}  // namespace

ProgramAst* parse(const char* source, char* err, size_t errcap, uint16_t* err_off) {
  if (err && errcap > 0) err[0] = '\0';
  if (err_off) *err_off = 0;

  ProgramAst* ast = new ProgramAst();

  Lexer lx(source ? source : "");

  // Empty source → error.
  if (lx.peek().type == TOK_EOF) {
    return fail(ast, "empty DSL source", 0, err, errcap, err_off);
  }

  for (;;) {
    if (!parseWheel(lx, ast, err, errcap, err_off)) {
      delete ast;
      return nullptr;
    }
    // Trap lexer errors mid-stream.
    if (lx.hasError()) {
      Token et = lx.peek();
      return fail(ast, lx.errorMsg(), et.offset, err, errcap, err_off);
    }
    Token p = lx.peek();
    if (p.type == TOK_COLON) {
      (void)lx.next();
      continue;
    }
    if (p.type == TOK_EOF) break;
    if (p.type == TOK_ERROR) {
      return fail(ast, lx.errorMsg(), p.offset, err, errcap, err_off);
    }
    // Unexpected separator at top level.
    return fail(ast, expectedMsg("':' or end of input", p), p.offset, err, errcap, err_off);
  }

  return ast;
}

void freeProgramAst(ProgramAst* ast) {
  if (!ast) return;
  delete ast;
}
