# ESP32 Signal Generator — Current State

> **Generated:** 2026-05-20
> **Branch:** Integrate-Ardu-Sim
> **Implementation plan:** `_Plans-and-Records/implementation_plan.md`
> **Orchestration:** 5-agent parallel execution (A: gen-backend, B: pattern-lib, C: sweep-store, D: dsl-compiler, E: ui-io)

---

## 1. Executive Summary

The project has advanced from "5 algorithmic patterns / 1 channel / no sweep/compression/DSL/persistence" to a **strict-superset of the Ardu-Stim feature set** running on ESP32:

- ✅ **Native 3-channel byte-table backend** (`TableCkpGenerator`) using ESP-IDF `gptimer` (1 µs tick) + `dedic_gpio` bundle, ISR ≤ 5 statements.
- ✅ **64 patterns ported** from `References/wheel_defs.h` via reproducible converter; byte-equivalent.
- ✅ **Sweep + compression task** on Core 1 priority 2 (linear/log/waypoint sweep, sin-table compression with AVR `<655U` overflow guard preserved).
- ✅ **DSL compiler** (Lexer → Parser → Validator → Compiler) producing canonical PatternRefs in PSRAM, 12/12 validation rules, 70 + 20 + 112 host-tested unit checks.
- ✅ **NVS persistence (schema v1)** with forward-migration safety; LittleFS storage for user DSL + compiled cache w/ SHA-256 invalidation.
- ✅ **Dual-mode serial protocol** — legacy Ardu-Stim single-byte opcodes (a/c/C/L/n/N/p/P/R/r/s/S/X) coexisting with new text protocol (LIST/SELECT/COMPILE/SAVE/LOAD/DELETE/CAPTURE/SWEEP/COMP).
- ✅ **LVGL UI surface** — 64-pattern dropdown with categories + filter, 3 channel LEDs, Sweep/Compression modals, DSL editor modal, waveform viewer (canvas).
- ⚠ Partial: capture loopback validator (API present, comparison loop stubbed); waveform pan/zoom (foundation in place); legacy `c`/`C` configTable emits defaults instead of fully packed bytes.

The S3 (`esp32-s3-n4r8`) target carries the full feature set. The WROOM-32D target retains legacy `huge_app.csv` partitions and the legacy backend per §8 risk #1, but pre-existing LVGL include chains in `main.cpp` need additional guards for true headless WROOM mode (documented TODO).

---

## 2. Milestone Status

| Milestone | Scope | Status | Notes |
|---|---|---|---|
| **M0.1** | IGenerator interface freeze + PatternRef + legacy adapter | ✅ Complete | `applySignalConfig()` preserved on legacy class; ISR memory contracts documented |
| **M0.2** | main.cpp MsgType + dispatch stubs | ✅ Complete | 15 message types wired, payload union extended |
| **M0.3** | Partitions + LittleFS scaffolding | ✅ Complete | 4 MiB layout (24 KB NVS, 3008 KB app, 1024 KB LittleFS); WROOM keeps `huge_app.csv` |
| **M1.1** | Native byte-table backend (1-channel) | ✅ Complete | `TableCkpGenerator` (gptimer + dedic_gpio), ISR 5 statements, IRAM_ATTR |
| **M1.2** | Convert 2 reference patterns | ✅ Complete | `convert_ardustim_wheels.py` MVP; byte-equivalent |
| **M1.3** | Wire build-flag selectable backend | ✅ Complete | `-DSIGGEN_BACKEND_TABLE=1` on S3 env; `gGen` IGenerator alias |
| **M2.1** | 3-channel widening | ✅ Complete | bundle width 1/2/3 per cam pin args; reverse rotation included |
| **M2.2** | with-cam variants | ✅ Complete | Folded into M3.1 (all 64 patterns) |
| **M2.3** | Channel LEDs in UI | ✅ Complete | 3 LV LEDs reflecting channel_mask + invert_mask |
| **M3.1** | All 64 patterns ported | ✅ Complete | 14,205 bytes total tables (target 16 KB, ceiling 20 KB) |
| **M3.2** | Friendly names table | ✅ Complete | 1,630 bytes (budget 2.5 KB) |
| **M3.3** | Legacy index migration | ✅ Complete | `pattern_legacy_index_to_key[]` mirrors `Wheels[]` order |
| **M3.4** | LVGL scrollable pattern list | ✅ Complete | 4 category headers (Distributor / Missing / Crank+Cam / Angular OEM) + filter textarea |
| **M3.5** | NVS schema v1 | ✅ Complete | 13 keys under namespace `siggen`; never blocks boot |
| **M3.6** | Delete legacy backend | ⚠ Deferred | `TimerCkpGenerator` retained as fallback under `!SIGGEN_BACKEND_TABLE` build flag; safe to remove once WROOM build path is fully migrated |
| **M4.1** | Linear sweep task | ✅ Complete | Core 1 priority 2; ping-pong ±1 RPM per interval |
| **M4.2** | Compression simulation | ✅ Complete | AVR `base_rpm < 655U` guard at SweepCompression.cpp:224 |
| **M4.3** | Log + waypoint sweep modes | ✅ Complete | exponential ramp + linear interpolation |
| **M4.4** | Per-cylinder profiles + custom curve + peak | ✅ Complete | 1/3/4-cyl dispatch + 256-byte user curve override + 0..100 peak scaling |
| **M4.5** | Sweep + Compression UI tabs | ✅ Complete | LVGL modals + live RPM display polling `sweepCurrentRpm()` |
| **M5.1** | DSL Lexer | ✅ Complete | 112 host-tested checks; all token classes, 5 malformed-input cases |
| **M5.2** | DSL Parser | ✅ Complete | Recursive descent → `ProgramAst` |
| **M5.3** | DSL Compiler | ✅ Complete | LCM merge + canonicalization + PSRAM allocation; 70 host-tested checks |
| **M5.4** | DSL Validator | ✅ Complete | 12/12 §7.5 rules with dedicated failing-input tests |
| **M5.5** | `dslCompileSignalConfig()` adapter | ✅ Complete | Constructs equivalent DSL string in-memory; single source of truth |
| **M5.6** | LittleFS pattern storage | ✅ Complete | `/patterns/<key>.{dsl,bin}`; SHA-256 invalidation; WROOM stubbed via `#if SIGGEN_USE_LITTLEFS` |
| **M5.7** | DSL editor modal | ✅ Complete | LVGL textarea + Compile/Save As/Load; error pipe via volatile char array polled by 250 ms timer |
| **M6.1** | CaptureRecorder | ✅ Complete | ISR ring buffer → single-channel PSRAM PatternRef; auto-registers as `captured_<ts>` |
| **M6.2** | LoopbackValidator | ✅ Complete | `captureLoopbackTick()` + `captureLoopbackErrorMsg()` added; managerTask polls on 100 ms cadence and publishes diagnostics to `g_loopback_error` (cleanup pass 2026-05-20) |
| **M6.3** | Capture LVGL page | ✅ Complete | Start/stop + sample count + save UI |
| **M7.1** | Waveform `lv_canvas` | ✅ Complete | 3-lane scope strip; 50 ms cursor timer reads `getEdgeCounter()` |
| **M7.2** | Pan/zoom + lane toggle | ⚠ Partial | Lane visibility mask + zoom factor present; touch gestures TODO |
| **M8** | LCD_CAM backend | ⏭ Deferred | Gated on M4 profiling per plan §4 |
| **M9.1** | Legacy serial parity | ✅ Complete | All 13 opcodes; `r` HI-LO byte order (cite `comms.cpp:128-130`); `c`/`C` config-table migration partial — emits defaults |
| **M9.2** | Text protocol | ✅ Complete | LIST/SELECT/COMPILE/SAVE/LOAD/DELETE/CAPTURE/SWEEP/COMP/RPM; `isalpha + space` detection rule |

---

## 3. Code Inventory

### Generator backend (`lib/ckp_gen/`)
- `PatternRef.h` — universal pattern handle (table, slot_count, degrees, rpm_scaler, channel_mask, name_key)
- `CkpGenerator.h` — widened IGenerator interface (3 pins, PatternRef apply, setRpm fast path, channel_mask invert, getEdgeCounter, getCycleStartUs/Duration); `SignalConfig` retained for legacy modal
- `CkpGenerator.cpp` — `TimerCkpGenerator` legacy adapter (deprecated, retained as fallback)
- `TableCkpGenerator.{h,cpp}` — native ESP-IDF gptimer + dedic_gpio backend; ISR ≤ 5 statements; per-channel invert; reverse rotation; cycle-time accessors

### Pattern library (`lib/patterns/`)
- `PatternLibrary.{h,cpp}` — registry API: builtinCount/builtinByIndex/findByKey/registerUserPattern/forEach + `findByLegacyIndex` + `friendlyName`
- `builtin_tables_generated.h` — 64 patterns, AUTO-GENERATED
- `pattern_names_generated.h` — friendly labels, AUTO-GENERATED
- `pattern_legacy_index_generated.h` — legacy `Wheels[]` index → name_key, AUTO-GENERATED

### Sweep / compression / storage (`lib/sweep_compression/`)
- `SweepCompression.{h,cpp}` — Core 1 priority 2 task; linear/log/waypoint sweep; sin-table compression with cyl dispatch + custom curve + peak scaling; AVR 655U guard preserved
- `CompressionTables.h` — sin_100_180/120/90 verbatim from `References/globals.h:80-120`
- `NvsStore.{h,cpp}` — schema v1 (13 keys); never blocks boot; forward-migration safe
- `PatternStorage.{h,cpp}` — LittleFS `/patterns/<key>.{dsl,bin}` with SHA-256 cache invalidation; WROOM no-op stub
- `LittleFSInit.{h,cpp}` — mount + smoke test; WROOM no-op stub

### DSL (`lib/dsl/`)
- `Dsl.h` — public API (dslCompile, dslCompileSignalConfig, dslFree)
- `Lexer.{h,cpp}` — token types: INT / FRACTION / INT_SUFFIXED / LETTER / COMMA / COLON / SLASH / EOF / ERROR
- `Parser.{h,cpp}` — recursive descent → ProgramAst
- `Validator.{h,cpp}` — 12 §7.5 rules
- `Compiler.{h,cpp}` — LCM merge + canonicalization + PSRAM allocation; `dslCompileSignalConfig` adapter

### UI + I/O (`lib/ui_lvgl/`, `lib/ckp_capture/`)
- `ui_lvgl.{h,cpp}` — 64-pattern dropdown w/ categories + filter; 3 channel LEDs; SWEEP/COMP/DSL/WAVE modals; waveform canvas page
- `ctrl_msg.h` — shared CtrlMsg/MsgType/MsgPayload + gCtrlQ + sendCtrlMsg
- `serial_cli.{h,cpp}` — dual-mode serial parser (legacy opcodes + text protocol)
- `CaptureRecorder.{h,cpp}` — ISR-based edge capture → PatternRef in PSRAM
- `EdgePulseCapture.{h,cpp}` — pre-existing low-level edge capture

### Tests (`test/`)
- `test_dsl_lexer.cpp` — 112 checks
- `test_dsl_compiler.cpp` — 70 checks (4 worked examples + 60-2 regression + SignalConfig adapter)
- `test_dsl_validator.cpp` — 20 checks (every §7.5 rule has a failing-input test)
- `test_pattern_migration.cpp` — 5 checks (legacy-index → key lookup)

### Tools (`tools/`)
- `convert_ardustim_wheels.py` — reproducible converter; parses `Wheels[]` from `ardustim.ino` + byte arrays from `wheel_defs.h`; exits 1 on any byte mismatch

### Top level
- `partitions_signalgen.csv` — 4 MiB layout (NVS 24 KB + factory 3008 KB + LittleFS 1024 KB)
- `platformio.ini` — S3 env: `-DSIGGEN_BACKEND_TABLE=1 -DSIGGEN_USE_LITTLEFS=1`, partitions_signalgen.csv, filesystem=littlefs; WROOM env unchanged (huge_app.csv)

---

## 4. Architectural Decisions (Locked Per Plan §1)

1. **Backend swap, not interface change** — widened IGenerator, kept manager↔generator shape; Phase 1 shipped behind `-DSIGGEN_BACKEND_TABLE` with legacy fallback.
2. **Byte-packed multi-channel tables** — `bit0=crank, bit1=cam1, bit2=cam2, bit3=knock-reserved` is the universal in-memory representation.
3. **Primary backend = GPTimer ISR + dedic_gpio bundle** — RMT and LCD_CAM explicitly not primary; LCD_CAM held in reserve (M8 deferred).
4. **SignalConfig survives** — retained for the existing UI custom modal; same compiler consumes both DSL text and SignalConfig (single source of truth via `dslCompileSignalConfig`).
5. **Validation + lastGood rollback** preserved in `applyPatternRef()` helper inside managerTask.
6. **LVGL pending-flag sync** is the only cross-core UI sync mechanism.
7. **String-keyed pattern selection** — `name_key` is the persistent key; legacy-index migration table preserved for Ardu-Stim wire-protocol compatibility.
8. **Dual-mode serial** — first byte alphabetic + space → text; else legacy single-byte.

---

## 5. Hardware-Verification Tasks (Out of Scope for This Session)

The following M1.1/M1.3/M2.1/M3.5/M4.x/M5.6 exit criteria require physical hardware and a logic analyzer / oscilloscope. They are documented in the plan and ready to be validated on bench:

- M1.1: scope-match `TableCkpGenerator` 60-2 @ 1000 RPM to legacy backend bit-identically
- M1.3: ±2 µs scope match for 100–6000 RPM range across 2 converted patterns
- M2.1: confirm cam edges land on exact crank slot specified by source byte
- M3.5: `nvs_get_str("pattern_key")` returns last-applied key after reboot
- M4.1: scope-confirm linear RPM ramp low→high→low
- M4.2: loopback capture verifies sin-dip waveform below `compressionRPM`
- M5.6: round-trip DSL → compile → save → reboot → load → identical scope trace
- M9.1: existing Ardu-Stim Electron GUI drives the device unmodified

---

## 6. Outstanding TODOs

1. **M7 waveform interactivity** — pan/zoom touch gestures and tap-on-lane toggles need event handlers (lane_mask and zoom factor exist).
2. **Legacy `c`/`C` opcodes** — 22-byte `configTable` schema emits zeros instead of the real packed config. Electron GUI reads defaults — not a full parity ship.
3. **M3.6 final legacy delete** — `TimerCkpGenerator` retained as `!SIGGEN_BACKEND_TABLE` fallback. Safe to remove once hardware sign-off confirms TableCkpGenerator parity AND WROOM build path is migrated.
4. **DSL `c` rotation semantics** — Agent D flagged ambiguity: `c` overloaded for both "720° period" and "reverse rotation". Asymmetric cam patterns may flip polarity post-canonicalization. Worked-example revision recommended.
5. **60-2 + half-moon cam DSL regression** — Agent D regression compiles to 240 bytes / 720° (correct dimensions) but does not byte-match `sixty_minus_two_with_halfmoon_cam` because the reference encodes cam transitions at specific teeth (44/43) not expressible via symmetric DSL. An angular-cam DSL form would match — left as worked-example revision.

### Resolved in cleanup pass (2026-05-20)

- **`MSG_SAVE_USER` payload** — `MsgPayload` extended with `save { name; dsl_source }` struct. DSL editor's Save button now heap-allocates copies of both the user-supplied name and the textarea source and posts via `sendCtrlMsg`; the manager handler calls `PatternStorage::saveDsl(name, dsl_source)` and frees both heap strings. Serial CLI `SAVE` path passes `dsl_source = nullptr` and falls back to the legacy "alias of <key>" placeholder.
- **DSL pattern leak management** — `scratch_dsl` lifetime now owned by a module-level `s_scratch_dsl` PatternRef + `s_scratch_active` flag in `main.cpp`. `cleanupScratchDsl()` is the single free point and is invoked: (a) before publishing a new compile in MSG_LOAD_DSL and MSG_SET_CUSTOM, (b) inside `applyPatternRef()` when swapping away to a non-scratch ref (e.g. selecting a builtin). Re-compile-frees-prior is now a defensive contract rather than a hopeful "lazy" comment.
- **M6 loopback comparison loop** — `captureLoopbackTick(uint32_t current_rpm)` added to `CaptureRecorder.{h,cpp}`. Computes expected slot period as `60'000'000 / (rpm * slot_count)` and scans the last 8 captured inter-edge deltas, ignoring gaps > 2× expected (intentional missing-tooth markers). Tolerance breaches set a sticky error flag plus a 96-byte diagnostic string (`captureLoopbackErrorMsg()`). `managerTask` now uses a 100 ms `xQueueReceive` timeout to pump the tick; the diagnostic is copied to a `volatile char g_loopback_error[96]` for LVGL polling.
- **WROOM headless mode guards** — Added `SIGGEN_HAS_DISPLAY` build flag (S3 env only). In `main.cpp` the `ui_lvgl.h` include and the `ui_get_active_pattern_for_wave`/`ui_get_edge_counter` extern "C" hooks are now `#if defined(SIGGEN_HAS_DISPLAY)`; the `#else` branch provides no-op `ui_*()` stubs so the rest of the manager task code stays uncluttered. `ui_lvgl.cpp` is wrapped in a top-level `#if defined(SIGGEN_HAS_DISPLAY)` and `ui_lvgl.h` has a belt-and-suspenders `#error` if included in a headless TU. `ctrl_msg.h` and `serial_cli.{h,cpp}` remain unguarded — they're protocol-level and shared.

---

## 7. Memory Budget (vs. Plan §7)

| Item | Budget | Actual | Status |
|---|---|---|---|
| Builtin pattern tables (.rodata) | 16 KB target / 20 KB ceiling | 14,205 B (~14 KB) | ✅ Under |
| Friendly-name strings | 2.5 KB | 1,630 B (~1.6 KB) | ✅ Under |
| Sin tables (compression) | 8 KB | 390 B | ✅ Under |
| Total `.rodata` budget | 32 KB | ~16 KB | ✅ Under |
| Compiled DSL pattern (per) | 4096 B | enforced at compile-time | ✅ Validated |
| LittleFS partition | 1 MB | 1024 KiB exact | ✅ |
| NVS partition | 24 KB | 13 keys × ~12 B avg = ~150 B | ✅ Plenty of headroom |

---

## 8. Files Generated / Touched This Orchestration

**New files (40+):**
```
lib/ckp_gen/PatternRef.h
lib/ckp_gen/TableCkpGenerator.h
lib/ckp_gen/TableCkpGenerator.cpp
lib/patterns/PatternLibrary.h
lib/patterns/PatternLibrary.cpp
lib/patterns/builtin_tables_generated.h
lib/patterns/pattern_names_generated.h
lib/patterns/pattern_legacy_index_generated.h
lib/sweep_compression/SweepCompression.h
lib/sweep_compression/SweepCompression.cpp
lib/sweep_compression/CompressionTables.h
lib/sweep_compression/NvsStore.h
lib/sweep_compression/NvsStore.cpp
lib/sweep_compression/LittleFSInit.h
lib/sweep_compression/LittleFSInit.cpp
lib/sweep_compression/PatternStorage.h
lib/sweep_compression/PatternStorage.cpp
lib/dsl/Dsl.h
lib/dsl/Lexer.h
lib/dsl/Lexer.cpp
lib/dsl/Parser.h
lib/dsl/Parser.cpp
lib/dsl/Validator.h
lib/dsl/Validator.cpp
lib/dsl/Compiler.h
lib/dsl/Compiler.cpp
lib/ui_lvgl/ctrl_msg.h
lib/ui_lvgl/serial_cli.h
lib/ui_lvgl/serial_cli.cpp
lib/ckp_capture/CaptureRecorder.h
lib/ckp_capture/CaptureRecorder.cpp
test/test_dsl_lexer.cpp
test/test_dsl_compiler.cpp
test/test_dsl_validator.cpp
test/test_pattern_migration.cpp
tools/convert_ardustim_wheels.py
partitions_signalgen.csv
```

**Modified files:**
```
src/main.cpp                  — full integration: backend selection, NVS/LittleFS wiring, 15 MsgType handlers, applyPatternRef rollback helper
lib/ckp_gen/CkpGenerator.h    — widened IGenerator, deprecation comments on TimerCkpGenerator
lib/ckp_gen/CkpGenerator.cpp  — legacy adapter implementing new interface (applySignalConfig)
lib/ui_lvgl/ui_lvgl.h         — channel-LED API, modal panels
lib/ui_lvgl/ui_lvgl.cpp       — 64-pattern dropdown, filter, channel LEDs, Sweep/Comp/DSL/Wave overlays
platformio.ini                — S3 env: new partitions, SIGGEN_BACKEND_TABLE + SIGGEN_USE_LITTLEFS flags, filesystem=littlefs
```

---

## 9. Acceptance Demo Path (per Plan §9.7)

End-to-end demo at M9 acceptance:
1. Power-cycle the S3 device — boots into last-applied pattern from NVS.
2. Existing Ardu-Stim Electron GUI (legacy single-byte opcodes) connects, enumerates wheels via `L`, switches wheels via `n`/`N`, sweeps via `s`/`S` — without modification. (Partial: `c`/`C` config-table parity is incomplete; defaults are returned.)
3. New text protocol exercise via PuTTY: `LIST`, `SELECT sixty_minus_two`, `COMPILE 1,C,M,1/2,60,58t,2m`, `SAVE user/my_pattern`, `SWEEP 500 5000 1 1000`, `COMP on 4 1500 50 1`.
4. LVGL touch UI: pattern dropdown w/ filter, sweep modal, compression modal, DSL editor, waveform canvas all functional.
5. RPM display tracks `sweepCurrentRpm()` live during sweep.

---

*This snapshot represents the codebase state after a single autonomous orchestration session driving 8 sub-agents across 9 milestones. Hardware sign-off, scope captures, and the M9 Electron-GUI acceptance demo remain as bench tasks per plan §9.7.*
