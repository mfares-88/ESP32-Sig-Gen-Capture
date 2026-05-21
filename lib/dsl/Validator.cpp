// Validator.cpp — implements the 12 semantic rules from §7.5 of the
// integration report. Rule numbering is preserved.
//
//  #1  Pin in {1,2,3,4}; unique within group.
//  #2  1–4 wheels per group.
//  #3  <kind> tail shape matches kind.
//  #4  Duty n/d: 0 < n < d, d ≤ 32.
//  #5  Symmetric/Missing slot count ≥ 2.
//  #6  Missing wheel: Σ t-counts + Σ m-counts == total_teeth, at least
//      one 'm' entry.
//  #7  Angular crank sums to exactly 360; cam sums to 720.
//  #8  Angular entries positive.
//  #9  Computed L ≤ 4096 bytes (pre-bound: per-wheel ≤ 4096 too).
// #10  Channel mask non-zero. (Trivially true when ast has ≥1 wheel and
//      rule #1 passed; we still assert.)
// #11  DSL source ≤ 512 chars.
// #12  No duplicate identical wheel definitions within a group.

#include "Validator.h"
#include "Parser.h"

#include <stdio.h>
#include <string.h>

namespace {

ValidationResult mk(uint8_t rule, uint16_t off, const char* msg) {
  ValidationResult r{};
  r.ok         = false;
  r.rule       = rule;
  r.src_offset = off;
  size_t n = strlen(msg);
  if (n >= sizeof(r.message)) n = sizeof(r.message) - 1;
  memcpy(r.message, msg, n);
  r.message[n] = '\0';
  return r;
}

// Returns the duty denominator multiplier for slot expansion.
// For Symmetric/Missing patterns, each tooth occupies `duty_den` slots.
// Per §7.2: slots = total_teeth × duty_den.
uint32_t expandedSlotCount(const WheelDef& w) {
  if (w.kind == WheelKind::Symmetric || w.kind == WheelKind::Missing) {
    return uint32_t(w.total_teeth) * uint32_t(w.duty_den);
  }
  // Angular: one slot per degree (option 1 from §7.2 TODO L77-83).
  uint32_t s = 0;
  for (auto& a : w.angular) s += a.degrees;
  return s;
}

bool sameWheel(const WheelDef& a, const WheelDef& b) {
  if (a.kind != b.kind || a.rotation != b.rotation) return false;
  if (a.duty_num != b.duty_num || a.duty_den != b.duty_den) return false;
  if (a.total_teeth != b.total_teeth) return false;
  if (a.runs.size() != b.runs.size()) return false;
  for (size_t i = 0; i < a.runs.size(); ++i) {
    if (a.runs[i].value != b.runs[i].value) return false;
    if (a.runs[i].suffix != b.runs[i].suffix) return false;
  }
  if (a.angular.size() != b.angular.size()) return false;
  for (size_t i = 0; i < a.angular.size(); ++i) {
    if (a.angular[i].degrees != b.angular[i].degrees) return false;
  }
  return true;
}

}  // namespace

ValidationResult validate(const ProgramAst* ast, size_t source_len) {
  // Rule #11: source length ≤ 512.
  if (source_len > 512) {
    return mk(11, 0, "DSL source exceeds 512 characters");
  }

  if (!ast) {
    return mk(2, 0, "no wheels in group");
  }
  const auto& wheels = ast->wheels;

  // Rule #2: 1..4 wheels.
  if (wheels.empty()) {
    return mk(2, 0, "no wheels in group");
  }
  if (wheels.size() > 4) {
    return mk(2, 0, "more than 4 wheels in group");
  }

  // Per-wheel checks.
  uint8_t pin_seen_mask = 0;
  uint32_t L_lcm = 1;  // accumulator for rule #9 pre-check

  for (size_t i = 0; i < wheels.size(); ++i) {
    const WheelDef& w = wheels[i];
    const uint16_t off = w.src_offset;

    // Rule #1: pin in {1,2,3,4}, unique.
    if (w.pin < 1 || w.pin > 4) {
      return mk(1, off, "pin must be 1..4");
    }
    const uint8_t bit = uint8_t(1u << (w.pin - 1));
    if (pin_seen_mask & bit) {
      return mk(1, off, "duplicate pin within group");
    }
    pin_seen_mask = uint8_t(pin_seen_mask | bit);

    // Rule #3: <kind> tail shape matches kind.
    if (w.kind == WheelKind::Symmetric) {
      if (w.total_teeth == 0 || !w.runs.empty() || !w.angular.empty()) {
        return mk(3, off, "symmetric tail shape mismatch");
      }
    } else if (w.kind == WheelKind::Missing) {
      if (w.total_teeth == 0 || w.runs.empty() || !w.angular.empty()) {
        return mk(3, off, "missing tail shape mismatch");
      }
    } else {  // Angular
      if (w.angular.empty() || !w.runs.empty()) {
        return mk(3, off, "angular tail shape mismatch");
      }
    }

    // Rule #4: duty n/d: 0 < n < d, d ≤ 32. (Angular has no duty.)
    if (w.kind == WheelKind::Symmetric || w.kind == WheelKind::Missing) {
      if (w.duty_num <= 0 || w.duty_den <= 0 ||
          w.duty_num >= w.duty_den || w.duty_den > 32) {
        return mk(4, off, "duty must satisfy 0 < n < d and d <= 32");
      }
    }

    // Rule #5: Symmetric/Missing slot count ≥ 2.
    if (w.kind == WheelKind::Symmetric || w.kind == WheelKind::Missing) {
      const uint32_t slots = expandedSlotCount(w);
      if (slots < 2) {
        return mk(5, off, "wheel slot count below minimum of 2");
      }
    }

    // Rule #6: Missing: Σt + Σm == total_teeth, at least one 'm'.
    if (w.kind == WheelKind::Missing) {
      if (uint32_t(w.teeth_with) + uint32_t(w.missing) != uint32_t(w.total_teeth)) {
        return mk(6, off, "missing wheel run-list does not sum to total teeth");
      }
      if (w.missing == 0) {
        return mk(6, off, "missing wheel requires at least one 'm' entry");
      }
    }

    // Rule #7: Angular crank sums to 360; cam sums to 720.
    // Rule #8: Angular entries positive.
    if (w.kind == WheelKind::Angular) {
      uint32_t sum = 0;
      for (auto& a : w.angular) {
        if (a.degrees == 0) {
          return mk(8, off, "angular entries must be positive");
        }
        sum += a.degrees;
      }
      const uint32_t want = (w.rotation == Rotation::CW) ? 360u : 720u;
      if (sum != want) {
        return mk(7, off, "angular durations do not sum to required total");
      }
    }

    // Rule #9 pre-bound: per-wheel slots ≤ 4096.
    const uint32_t slots = expandedSlotCount(w);
    if (slots > 4096) {
      return mk(9, off, "per-wheel slot count exceeds 4096");
    }
    // Approximate LCM accumulator. We compute exactly for pre-validation
    // but the Compiler will re-check the post-cam-doubling LCM.
    uint32_t a = L_lcm, b = slots;
    while (b != 0) { uint32_t r = a % b; a = b; b = r; }
    uint32_t g = a ? a : 1;
    uint64_t lcm = (uint64_t(L_lcm) / g) * uint64_t(slots);
    if (lcm > 4096) {
      return mk(9, off, "compiled buffer would exceed 4096 bytes");
    }
    L_lcm = uint32_t(lcm);
  }

  // Rule #10: channel mask non-zero.
  if (pin_seen_mask == 0) {
    return mk(10, 0, "no active channels (empty group)");
  }

  // Rule #12: no duplicate identical wheel defs.
  for (size_t i = 0; i < wheels.size(); ++i) {
    for (size_t j = i + 1; j < wheels.size(); ++j) {
      if (sameWheel(wheels[i], wheels[j])) {
        return mk(12, wheels[j].src_offset, "duplicate identical wheel definition");
      }
    }
  }

  ValidationResult ok{};
  ok.ok = true;
  ok.rule = 0;
  ok.src_offset = 0;
  ok.message[0] = '\0';
  return ok;
}
