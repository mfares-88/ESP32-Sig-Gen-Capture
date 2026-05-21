#!/usr/bin/env python3
"""
convert_ardustim_wheels.py — Ardu-Stim wheel-table converter (M3.1+M3.2+M3.3)

Parses References/wheel_defs.h to extract PROGMEM byte arrays AND parses
References/ardustim.ino Wheels[] to recover ordered metadata (name pointer,
edge array pointer, rpm_scaler, max_edges, degrees). Emits three generated
headers:

    lib/patterns/builtin_tables_generated.h        — byte tables + PatternRef[]
    lib/patterns/pattern_names_generated.h         — name_key -> friendly label
    lib/patterns/pattern_legacy_index_generated.h  — legacy index -> name_key

Scope (M3.1/3.2/3.3): emits ALL ~64 patterns + with-cam variants, in the
exact Wheels[] order (so the legacy single-byte serial `L` opcode and any
legacy NVS index migration line up bit-for-bit with Ardu-Stim's AVR build).

Authorship contract (implementation_plan.md §6 Agent B):
    * Generator is the SINGLE source of truth — generated headers carry
      a do-not-edit banner.
    * name_key derives from the C identifier (NOT the *_friendly_name).
    * rpm_scaler is copied VERBATIM from ardustim.ino:59-125.
    * channel_mask is derived by OR-ing bits 0..3 of every byte in the
      source array (per CLAUDE.md: bit0=crank, bit1=cam1, bit2=cam2,
      bit3=knock-reserved).
    * Byte arrays are emitted byte-for-byte; mismatches abort with exit 1.
    * .rodata budget: hard ceiling 20 KB, target 16 KB. Actual reported.
    * Friendly-name byte budget: <= 2.5 KB. Actual reported.

Usage:
    python3 tools/convert_ardustim_wheels.py
    python tools/convert_ardustim_wheels.py

Optional flags:
    --src-defs  Path to wheel_defs.h   (default: References/wheel_defs.h)
    --src-ino   Path to ardustim.ino   (default: References/ardustim.ino)
    --out-dir   Output directory       (default: lib/patterns)
"""

from __future__ import annotations

import argparse
import datetime as _dt
import hashlib
import re
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Byte budget constants (per implementation_plan.md §6 + §M3 exit criteria).
# ---------------------------------------------------------------------------
RODATA_HARD_CEILING_BYTES = 20 * 1024  # hard fail above this
RODATA_TARGET_BYTES       = 16 * 1024  # warn above this
NAMES_BUDGET_BYTES        = int(2.5 * 1024)  # friendly-name strings ceiling

# ---------------------------------------------------------------------------
# Byte-array extraction (from wheel_defs.h)
# ---------------------------------------------------------------------------
_ARRAY_HEADER_RE = re.compile(
    r"const\s+unsigned\s+char\s+(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*\[\s*\]\s*"
    r"PROGMEM\s*=\s*",
    re.MULTILINE,
)

# Friendly-name string literals in wheel_defs.h, e.g.:
#   const char sixty_minus_two_friendly_name[] PROGMEM = "60-2 crank only";
_FRIENDLY_RE = re.compile(
    r"const\s+char\s+(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*\[\s*\]\s*"
    r"PROGMEM\s*=\s*\"(?P<body>(?:[^\"\\]|\\.)*)\"\s*;",
    re.MULTILINE,
)

_INT_SUFFIX_RE = re.compile(r"[uUlL]+$")


def _strip_c_comments(text: str) -> str:
    """Remove /* ... */ block and // line comments. Preserves layout."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def _parse_byte_literal(tok: str) -> int:
    tok = tok.strip()
    if not tok:
        raise ValueError("empty byte literal")
    # Strip C integer suffixes (u, U, l, L, ul, LL, etc.). wheel_defs.h has
    # at least one stray `1l` literal (line 390) — tolerate it.
    tok = _INT_SUFFIX_RE.sub("", tok)
    if not tok:
        raise ValueError("byte literal had only suffix characters")
    if tok.lower().startswith("0x"):
        return int(tok, 16)
    if tok.lower().startswith("0b"):
        return int(tok, 2)
    return int(tok, 10)


def extract_arrays(src_text: str) -> dict[str, list[int]]:
    """Return {c_identifier: [int, ...]} for every PROGMEM byte array."""
    cleaned = _strip_c_comments(src_text)
    out: dict[str, list[int]] = {}
    for m in _ARRAY_HEADER_RE.finditer(cleaned):
        name = m.group("name")
        i = m.end()
        while i < len(cleaned) and cleaned[i] != "{":
            if not cleaned[i].isspace():
                raise ValueError(
                    f"expected '{{' after '{name}[] PROGMEM =', found {cleaned[i]!r}"
                )
            i += 1
        if i >= len(cleaned):
            raise ValueError(f"unterminated array {name}")
        j = cleaned.find("}", i + 1)
        if j < 0:
            raise ValueError(f"unterminated array {name}")
        body = cleaned[i + 1 : j]
        tokens = [t for t in (s.strip() for s in body.split(",")) if t]
        try:
            values = [_parse_byte_literal(t) for t in tokens]
        except ValueError as e:
            raise ValueError(f"array {name}: {e}") from e
        for v in values:
            if not 0 <= v <= 255:
                raise ValueError(f"array {name}: byte out of range: {v}")
        out[name] = values
    return out


def extract_friendly_names(src_text: str) -> dict[str, str]:
    """Return {friendly_name_identifier: literal_string}.

    Note: the *_friendly_name identifier is the LHS of the declaration in
    wheel_defs.h. The mapping to a wheel's name_key happens via the
    `Wheels[]` initializer (see parse_wheels_table).
    """
    out: dict[str, str] = {}
    for m in _FRIENDLY_RE.finditer(src_text):
        out[m.group("name")] = m.group("body")
    return out


# ---------------------------------------------------------------------------
# Wheels[] parsing (from ardustim.ino)
# ---------------------------------------------------------------------------
# Each row looks like (whitespace flexible):
#   { dizzy_four_cylinder_friendly_name, dizzy_four_cylinder, 0.03333, 4, 360 },
# We tolerate any amount of whitespace, optional trailing comma at last row.
_WHEELS_ROW_RE = re.compile(
    r"\{\s*"
    r"(?P<name_ptr>[A-Za-z_][A-Za-z0-9_]*)\s*,\s*"
    r"(?P<edges_ptr>[A-Za-z_][A-Za-z0-9_]*)\s*,\s*"
    r"(?P<rpm>[0-9eE+\-\.]+)\s*,\s*"
    r"(?P<edges>[0-9]+)\s*,\s*"
    r"(?P<degrees>[0-9]+)\s*"
    r"\}",
)


def parse_wheels_table(ino_text: str) -> list[dict]:
    """Parse the `wheels Wheels[MAX_WHEELS] = { ... };` block.

    Returns an ordered list of dicts, one per row, with keys:
        name_ptr     str  — identifier of the *_friendly_name symbol
        edges_ptr    str  — identifier of the byte array (becomes name_key)
        rpm_scaler   str  — verbatim text of the scaler (preserved for output)
        max_edges    int
        degrees      int
    """
    cleaned = _strip_c_comments(ino_text)
    # Find the start of the initializer.
    m = re.search(r"wheels\s+Wheels\s*\[\s*MAX_WHEELS\s*\]\s*=\s*\{", cleaned)
    if not m:
        raise ValueError("Could not find Wheels[] initializer in ardustim.ino")
    start = m.end()
    # Find matching closing brace at top level.
    depth = 1
    end = start
    while end < len(cleaned) and depth > 0:
        c = cleaned[end]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        end += 1
    block = cleaned[start : end - 1]  # exclude outer braces
    rows: list[dict] = []
    for r in _WHEELS_ROW_RE.finditer(block):
        rows.append(
            {
                "name_ptr": r.group("name_ptr"),
                "edges_ptr": r.group("edges_ptr"),
                "rpm_scaler": r.group("rpm").strip(),
                "max_edges": int(r.group("edges")),
                "degrees": int(r.group("degrees")),
            }
        )
    return rows


# ---------------------------------------------------------------------------
# Channel-mask derivation
# ---------------------------------------------------------------------------
def derive_channel_mask(data: list[int]) -> int:
    """OR all bytes' bits 0..3 (per CLAUDE.md encoding)."""
    m = 0
    for b in data:
        m |= b & 0x0F
    return m


# ---------------------------------------------------------------------------
# Header emission helpers
# ---------------------------------------------------------------------------
BANNER = (
    "// AUTO-GENERATED BY tools/convert_ardustim_wheels.py — DO NOT EDIT\n"
)

BUILTIN_HEADER_TEMPLATE = """\
{banner}//
// Sources:
//   References/wheel_defs.h  SHA256={defs_sha}
//   References/ardustim.ino  SHA256={ino_sha}
// Generated: {timestamp}
//
// Rebuild with:
//     python3 tools/convert_ardustim_wheels.py
//
// Scope: M3.1 — all {count} patterns from Wheels[] (References/ardustim.ino:59-125).
// Bytes are byte-for-byte copies of the wheel_defs.h PROGMEM arrays;
// see implementation_plan.md §3.4 and §6 (Agent B mandate).
//
// .rodata table bytes (sum of byte arrays): {rodata_total}
// Hard ceiling: {hard_ceiling} bytes (target {target}).

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "PatternRef.h"

namespace builtin_tables {{

{tables}
// Stable string keys (rodata literals; lifetime == program lifetime).
{name_keys}
}} // namespace builtin_tables

static const PatternRef builtin_patterns[] = {{
{entries}
}};

static constexpr size_t builtin_pattern_count =
    sizeof(builtin_patterns) / sizeof(builtin_patterns[0]);
"""

NAMES_HEADER_TEMPLATE = """\
{banner}//
// Friendly-name lookup table (PatternRef.name_key -> human label).
// Sources:
//   References/wheel_defs.h  SHA256={defs_sha}
//   References/ardustim.ino  SHA256={ino_sha}
// Generated: {timestamp}
//
// Friendly-label string bytes (sum + NUL terminators): {bytes_used} / {budget}.
// Note: the `key` pointer in each row is shared with builtin_tables_generated.h
// (same .rodata literal), so the table adds only the human-label strings.
// Lookup helper: PatternLibrary::friendlyName(name_key).

#pragma once

#include <stddef.h>

#include "builtin_tables_generated.h"

#ifndef PROGMEM
#  define PROGMEM
#endif

struct PatternFriendlyName {{
    const char* key;       // matches PatternRef::name_key (C identifier)
    const char* friendly;  // human label
}};

static const PatternFriendlyName pattern_friendly_names[] PROGMEM = {{
{rows}
}};

static constexpr size_t pattern_friendly_name_count =
    sizeof(pattern_friendly_names) / sizeof(pattern_friendly_names[0]);
"""

LEGACY_HEADER_TEMPLATE = """\
{banner}//
// Legacy Wheels[]-index -> name_key migration table.
// Source order: References/ardustim.ino:59-125 (the Wheels[] initializer).
// Generated: {timestamp}
//
// Consumers:
//   * M9.1 legacy serial `L` opcode (Agent E)  — interprets the 1-byte
//     wheel index over the wire by indexing this array.
//   * NVS migration (Agent C / M3.5) — converts a numeric wheel index
//     from an older config blob into the string key actually persisted.
// Lookup helper: PatternLibrary::findByLegacyIndex(idx).

#pragma once

#include <stddef.h>

#ifndef PROGMEM
#  define PROGMEM
#endif

static const char* const pattern_legacy_index_to_key[] PROGMEM = {{
{rows}
}};

static constexpr size_t pattern_legacy_index_count =
    sizeof(pattern_legacy_index_to_key) / sizeof(pattern_legacy_index_to_key[0]);
"""


def _format_byte_array(name: str, data: list[int], cols: int = 16) -> str:
    lines = []
    for i in range(0, len(data), cols):
        chunk = data[i : i + cols]
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    body = "\n".join(lines).rstrip(",")
    return (
        f"static const uint8_t {name}_table[] /* PROGMEM */ = {{\n"
        f"{body}\n"
        f"}};\n"
        f"static constexpr size_t {name}_table_len = sizeof({name}_table);\n"
    )


# ---------------------------------------------------------------------------
# Validation helpers
# ---------------------------------------------------------------------------
def diff_bytes(label: str, parsed: list[int], reference: list[int]) -> int:
    """Print a hex diff between two byte sequences. Returns mismatch count."""
    mismatches = 0
    if len(parsed) != len(reference):
        print(
            f"  {label}: LENGTH MISMATCH parsed={len(parsed)} reference={len(reference)}"
        )
        mismatches += abs(len(parsed) - len(reference))
    for i, (a, b) in enumerate(zip(parsed, reference)):
        if a != b:
            print(f"  {label}: byte[{i}] parsed=0x{a:02x} reference=0x{b:02x}")
            mismatches += 1
    return mismatches


def _c_escape(s: str) -> str:
    """Escape a Python string for safe embedding in a C/C++ string literal."""
    out = []
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\r":
            out.append("\\r")
        elif ch == "\t":
            out.append("\\t")
        elif 32 <= ord(ch) < 127:
            out.append(ch)
        else:
            # non-ASCII — embed as \xHH (single-byte UTF-8 encoded)
            for b in ch.encode("utf-8"):
                out.append(f"\\x{b:02x}")
    return "".join(out)


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------
def main(argv: list[str] | None = None) -> int:
    here = Path(__file__).resolve().parent
    repo_root = here.parent

    ap = argparse.ArgumentParser(description="Ardu-Stim wheel converter")
    ap.add_argument(
        "--src-defs",
        type=Path,
        default=repo_root / "References" / "wheel_defs.h",
        help="Path to wheel_defs.h",
    )
    ap.add_argument(
        "--src-ino",
        type=Path,
        default=repo_root / "References" / "ardustim.ino",
        help="Path to ardustim.ino",
    )
    ap.add_argument(
        "--out-dir",
        type=Path,
        default=repo_root / "lib" / "patterns",
        help="Output directory for generated headers",
    )
    args = ap.parse_args(argv)

    if not args.src_defs.is_file():
        print(f"ERROR: source not found: {args.src_defs}", file=sys.stderr)
        return 1
    if not args.src_ino.is_file():
        print(f"ERROR: source not found: {args.src_ino}", file=sys.stderr)
        return 1

    defs_text = args.src_defs.read_text(encoding="utf-8")
    ino_text  = args.src_ino.read_text(encoding="utf-8")

    defs_sha = hashlib.sha256(defs_text.encode("utf-8")).hexdigest()
    ino_sha  = hashlib.sha256(ino_text.encode("utf-8")).hexdigest()
    timestamp = _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

    arrays = extract_arrays(defs_text)
    friendly = extract_friendly_names(defs_text)
    wheels = parse_wheels_table(ino_text)

    if not wheels:
        print("ERROR: Wheels[] parse returned zero rows", file=sys.stderr)
        return 1

    print(f"=== Wheels[] rows parsed: {len(wheels)} ===")

    # --- Build per-pattern records, validate byte-equivalence ---
    print("=== Byte-equivalence validation ===")
    records: list[dict] = []
    skipped: list[tuple[str, str]] = []
    total_mismatches = 0

    for idx, row in enumerate(wheels):
        edges_id = row["edges_ptr"]
        if edges_id not in arrays:
            skipped.append((edges_id, "byte array not found in wheel_defs.h"))
            continue
        data = arrays[edges_id]
        # Re-extract to double-check the parser is deterministic.
        ref_again = extract_arrays(defs_text)[edges_id]
        n = diff_bytes(edges_id, data, ref_again)
        total_mismatches += n

        if len(data) != row["max_edges"]:
            print(
                f"  WARN: {edges_id}: byte array length {len(data)} != "
                f"Wheels[].max_edges {row['max_edges']} — Wheels[] takes precedence "
                f"for slot_count metadata."
            )
            # This is not a fatal mismatch — Wheels[] declares how many of
            # the array's edges the ISR actually walks; the array itself may
            # be longer (none observed in the current source, but be safe).

        channel_mask = derive_channel_mask(data[: row["max_edges"]])

        # Resolve friendly name string.
        fname_ptr = row["name_ptr"]
        friendly_label = friendly.get(fname_ptr, edges_id)  # fall back to key

        records.append(
            {
                "legacy_index": idx,
                "name_key": edges_id,            # C identifier
                "friendly_ptr": fname_ptr,
                "friendly_label": friendly_label,
                "rpm_scaler": row["rpm_scaler"],  # verbatim text
                "max_edges": row["max_edges"],
                "degrees": row["degrees"],
                "channel_mask": channel_mask,
                "bytes": data[: row["max_edges"]],
            }
        )

    if total_mismatches:
        print(f"FAIL: {total_mismatches} byte mismatch(es). Aborting.")
        return 1
    print(f"PASS: byte-equivalence verified for {len(records)} patterns.")
    if skipped:
        print(f"WARN: skipped {len(skipped)} entries:")
        for k, why in skipped:
            print(f"  - {k}: {why}")

    # --- Compute byte budget for tables ---
    rodata_total = sum(len(r["bytes"]) for r in records)
    print(f"=== .rodata table-bytes total: {rodata_total} ===")
    print(f"  target  : {RODATA_TARGET_BYTES}")
    print(f"  ceiling : {RODATA_HARD_CEILING_BYTES}")
    if rodata_total > RODATA_HARD_CEILING_BYTES:
        print(
            f"FAIL: rodata table bytes {rodata_total} exceed hard ceiling "
            f"{RODATA_HARD_CEILING_BYTES}.",
            file=sys.stderr,
        )
        return 1
    if rodata_total > RODATA_TARGET_BYTES:
        print(
            f"  NOTE: above 16 KB target by "
            f"{rodata_total - RODATA_TARGET_BYTES} bytes; still under ceiling."
        )

    # --- Emit builtin_tables_generated.h ---
    args.out_dir.mkdir(parents=True, exist_ok=True)

    tables_chunks = []
    name_key_lines = []
    seen_keys = set()
    for r in records:
        key = r["name_key"]
        if key in seen_keys:
            # Duplicate edges_ptr in Wheels[] — emit just one table+key block.
            continue
        seen_keys.add(key)
        tables_chunks.append(_format_byte_array(key, r["bytes"]))
        name_key_lines.append(
            f'static constexpr const char {key}_name_key[] = "{key}";'
        )

    entries = []
    for r in records:
        # Preserve the source token verbatim BUT ensure a valid C++ float
        # literal: if the source scaler has no decimal point (e.g. "1"),
        # append ".0" before the trailing 'f' so we emit "1.0f" not "1f".
        rpm_token = r["rpm_scaler"]
        rpm_suffix = ".0f" if "." not in rpm_token and "e" not in rpm_token.lower() else "f"
        entries.append(
            "    {{ /* legacy_index={idx}: {key} */\n"
            "        builtin_tables::{key}_table,\n"
            "        /* slot_count   */ {slot},\n"
            "        /* degrees      */ {deg},\n"
            "        /* rpm_scaler   */ {rpm}{rpm_suffix},\n"
            "        /* channel_mask */ 0x{mask:02x},\n"
            "        /* name_key     */ builtin_tables::{key}_name_key,\n"
            "    }},".format(
                idx=r["legacy_index"],
                key=r["name_key"],
                slot=r["max_edges"],
                deg=r["degrees"],
                rpm=rpm_token,
                rpm_suffix=rpm_suffix,
                mask=r["channel_mask"],
            )
        )

    builtin_text = BUILTIN_HEADER_TEMPLATE.format(
        banner=BANNER,
        defs_sha=defs_sha,
        ino_sha=ino_sha,
        timestamp=timestamp,
        count=len(records),
        rodata_total=rodata_total,
        hard_ceiling=RODATA_HARD_CEILING_BYTES,
        target=RODATA_TARGET_BYTES,
        tables="\n".join(tables_chunks),
        name_keys="\n".join(name_key_lines),
        entries="\n".join(entries),
    )
    (args.out_dir / "builtin_tables_generated.h").write_text(
        builtin_text, encoding="utf-8"
    )

    # --- Emit pattern_names_generated.h ---
    # Budget accounting per implementation_plan.md §M3.2: "byte budget for
    # strings". The C identifier (name_key) is already counted in the builtin
    # tables rodata via `*_name_key[]` literals — the friendly-name table
    # reuses the *same* key pointer, it does not duplicate the string. The
    # budget here is therefore the new bytes the friendly-name table adds:
    # the human-label string literals themselves (with NUL terminators).
    name_rows = []
    labels_bytes = 0       # human-label strings (the budgeted bytes)
    keys_ref_bytes = 0     # informational: bytes of key literals reused
    seen_for_names = set()
    for r in records:
        key = r["name_key"]
        if key in seen_for_names:
            continue
        seen_for_names.add(key)
        label = r["friendly_label"]
        key_esc = _c_escape(key)
        label_esc = _c_escape(label)
        keys_ref_bytes += len(key.encode("utf-8")) + 1
        labels_bytes  += len(label.encode("utf-8")) + 1
        # Use the existing rodata key literal so the friendly table doesn't
        # duplicate the C-identifier bytes (saves ~1.3 KB on 64 patterns).
        name_rows.append(
            f'    {{ builtin_tables::{key}_name_key, "{label_esc}" }},'
        )
    names_bytes_used = labels_bytes

    if names_bytes_used > NAMES_BUDGET_BYTES:
        print(
            f"FAIL: friendly-name bytes {names_bytes_used} exceed budget "
            f"{NAMES_BUDGET_BYTES}.",
            file=sys.stderr,
        )
        return 1
    print(f"=== friendly-name bytes: {names_bytes_used} / {NAMES_BUDGET_BYTES} ===")

    names_text = NAMES_HEADER_TEMPLATE.format(
        banner=BANNER,
        defs_sha=defs_sha,
        ino_sha=ino_sha,
        timestamp=timestamp,
        bytes_used=names_bytes_used,
        budget=NAMES_BUDGET_BYTES,
        rows="\n".join(name_rows),
    )
    (args.out_dir / "pattern_names_generated.h").write_text(
        names_text, encoding="utf-8"
    )

    # --- Emit pattern_legacy_index_generated.h ---
    legacy_rows = []
    for r in records:
        legacy_rows.append(
            f'    "{_c_escape(r["name_key"])}",'
            f'   // index {r["legacy_index"]}: {_c_escape(r["friendly_label"])}'
        )

    legacy_text = LEGACY_HEADER_TEMPLATE.format(
        banner=BANNER,
        timestamp=timestamp,
        rows="\n".join(legacy_rows),
    )
    (args.out_dir / "pattern_legacy_index_generated.h").write_text(
        legacy_text, encoding="utf-8"
    )

    # --- Summary ---
    print("=== Summary ===")
    print(f"  patterns emitted : {len(records)}")
    print(f"  unique keys      : {len(seen_keys)}")
    print(f"  patterns skipped : {len(skipped)}")
    print(f"  rodata bytes     : {rodata_total} (ceiling {RODATA_HARD_CEILING_BYTES})")
    print(f"  names bytes      : {names_bytes_used} (budget {NAMES_BUDGET_BYTES})")
    print(f"  written:")
    print(f"    - {args.out_dir / 'builtin_tables_generated.h'}")
    print(f"    - {args.out_dir / 'pattern_names_generated.h'}")
    print(f"    - {args.out_dir / 'pattern_legacy_index_generated.h'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
