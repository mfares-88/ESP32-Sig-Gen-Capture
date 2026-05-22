# ESP32 Signal Generator — Current State

> **Generated:** 2026-05-22 (Implementation-2 remediation pass)
> **Previous snapshot:** 2026-05-20 (preserved below in §11)
> **Branch:** Integrate-Ardu-Sim
> **Implementation plans:** `_Plans-and-Records/implementation_plan.md` (original), `_Plans-and-Records/implementation-2_plan.md` (review remediation)
> **Driver document:** `_Plans-and-Records/implementation-1_Review.md`
> **Orchestration:** 5-agent parallel model standing; Implementation-2 engaged A, C, E (B and D not needed this cycle)

---

## 1. Executive Summary

**Implementation-2 status (2026-05-22):** After first bench bring-up, a review (`implementation-1_Review.md`) found 12 defects. A 3-agent remediation pass (A / C / E) has landed: backend interface now returns `bool`+`GenError`, output pins are validated against an ESP32-S3 + JC4827W543 reserved set, a 24 KB internal DRAM playback buffer makes the ISR cache-safe during NVS / LittleFS writes, the main LVGL screen has been rebuilt on a 480×272 left/right-pane layout with no overlap, RPM arc events are coalesced (50 ms / on release), NVS RPM writes are debounced (750 ms), the LittleFS partition warning is gone, LVGL is pinned to `9.2.2` and GFX Library to `1.6.4`, a custom board manifest declares N4R8 (4 MB flash / 8 MB OPI PSRAM), and the generator is wired in **crank-only bring-up** behind a high-visibility `USER: SELECT BACKEND OUTPUT PINS` banner so the user can pick safe cam pins post-bench. Build is clean (0 warnings, RAM 31%, Flash 20%). Bench verification of the visible behavior is the next step.

**Original (2026-05-20) summary:** The project has advanced from "5 algorithmic patterns / 1 channel / no sweep/compression/DSL/persistence" to a **strict-superset of the Ardu-Stim feature set** running on ESP32:

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
| **R1.1** | Backend bool-return + GenError contract | ✅ Complete | `IGenerator::start/stop` return `bool`; `lastError()`/`isReady()` virtuals; UI shows specific failure strings instead of generic `Pattern apply failed` |
| **R1.2** | ESP32-S3 pin validation | ✅ Complete | `isValidEsp32S3OutputPin()` rejects S3-invalid pins, flash/PSRAM range, strapping pins, JC4827W543 board-reserved set; logs exact offending pin |
| **R1.3** | 24 KB DRAM playback buffer | ✅ Complete | `heap_caps_malloc(MALLOC_CAP_INTERNAL)` at `begin()`; ISR reads only DRAM → cache-safe during NVS/LittleFS writes. Tunable via `kPlaybackBufferBytes` (banner-commented) |
| **R1.4** | Crank-only bring-up | ✅ Complete | `gGen.begin(PIN_CKP_OUT, -1, -1)` wrapped in USER-SELECT-PINS banner in `src/main.cpp`. Macro values preserved for one-line user edit |
| **R2.1** | LVGL main-screen layout rebuild | ✅ Complete | Left pane (8,32,224,224) + right pane (240,12,232,252) containers; 2×2 action grid; INVERT/START bottom row; no widget overlap on 480×272 |
| **R2.2** | Display / touch rotation lock | ✅ Complete | `kDisplayRotation = 1` (hardcoded); auto-pick loop removed; boot Serial prints actual rotation + width/height + GT911 rotation |
| **R2.3** | RPM arc event coalescing | ✅ Complete | `on_arc_changed` fires `s_on_rpm()` only on `LV_EVENT_RELEASED` or 50 ms throttle; local label still updates per event |
| **R2.4** | Error label long-mode | ✅ Complete | `lbl_error` width 230, `LV_LABEL_LONG_DOT`, placed at (8,252,230,18) |
| **R2.5** | Spin row flex layout | ✅ Complete | `make_spin_row` rewritten with `LV_FLEX_FLOW_ROW` + column gap; right-aligned absolute offsets removed |
| **R3.1** | NVS RPM debounce | ✅ Complete | `NvsStore::setRpmDebounced/tickRpmDebounce/flushPendingRpm`; 750 ms; called from manager-task loop. Live `gGen.setRpm()` path unchanged |
| **R3.2** | Partition / LittleFS warning fix | ✅ Complete | Partition renamed `littlefs`→`spiffs` (name matches subtype); `LittleFS.begin(..., "spiffs")`; build emits no partition warning |
| **R3.3** | Custom board manifest + version pinning | ✅ Complete | New `boards/esp32-s3-n4r8.json` (4 MB flash / 8 MB OPI PSRAM); `lvgl@9.2.2` and `GFX Library@1.6.4` pinned exact |
| **R3.4** | Boot diagnostics | ✅ Complete | `setup()` prints flash size, PSRAM size, free internal heap, free PSRAM, then generator init result + last error |

> **R-series** milestones correspond to the 12 findings in `implementation-1_Review.md` as remediated by `implementation-2_plan.md`. Bench acceptance of these is the next gate.

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

### Implementation-2 additions (locked 2026-05-22)

9. **Backend errors are structured** — `IGenerator::start()/stop()/apply()` all surface `GenError` via `lastError()`. The UI translates these to specific human strings (`"Apply: GPIO invalid/reserved"`, `"Apply: pattern too large (>24KB)"`, etc.). Generic "Pattern apply failed" is forbidden going forward.
10. **ISR memory contract** — `TableCkpGenerator::apply()` MUST `memcpy` pattern bytes into the internal-DRAM `_playback_buffer` before pointing `_table` at it. ISR reads from internal DRAM only. Direct `.rodata` / PSRAM pointing into `_table` is forbidden — it breaks cache safety during NVS / LittleFS writes.
11. **Output pin choice is gated by `isValidEsp32S3OutputPin()`** — any pin offered to `gGen.begin()` is checked against the SoC valid set AND the JC4827W543 board-reserved set. A board change requires updating this allow-list, not loosening it.
12. **Display rotation is hardcoded, not auto-picked** — `kDisplayRotation` in `ui_lvgl.cpp` is the single source of truth. Touch rotation pairs with it; both must be flipped together if the board orientation changes.
13. **NVS commits for high-frequency UI events are debounced** — the LVGL arc / spinboxes call `NvsStore::setRpmDebounced` (and analogous helpers if added later), not raw `setRpm`. `tickRpmDebounce()` is polled from the manager task loop.

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

### Implementation-2 bench gates (new, 2026-05-22)

- **R1.1–R1.4 backend**: with `PIN_CKP_OUT` valid and cam pins `-1/-1`, pattern `sixty_minus_two` selects without error and START produces a valid 60-2 scope trace; STOP returns line low. Force `PIN_CAM1_OUT=21` and confirm UI shows `Apply: GPIO invalid/reserved` and START rejected.
- **R1.3 ISR safety**: while generator is running, repeatedly save NVS settings and trigger a LittleFS write — scope shows no malformed pulses.
- **R2.1 layout**: no overlapping rectangles on the main screen; INVERT and START have unique non-overlapping touch targets; pattern dropdown row taps select the touched row.
- **R2.2 rotation**: boot Serial prints expected rotation values; tapping four screen corners produces near-corner touch coordinates. If mirrored, swap `kDisplayRotation` / `kTouchRotation` per the comment in `ui_lvgl.cpp`.
- **R2.3 RPM coalescing**: drag the RPM arc full-range — no `Control queue full` in Serial; NVS write count is 1 per settled edit (not per arc step).
- **R3.4 boot diagnostics**: Serial shows flash size, `psram found=1 size≈8388608`, free internal heap > 100 KB, generator init result.
- **User action**: pick final cam GPIOs from the safe set documented in `TableCkpGenerator.cpp::isValidEsp32S3OutputPin` and edit the three lines under the `USER: SELECT BACKEND OUTPUT PINS` banner in `src/main.cpp`. Validate cam channels light their LEDs without disturbing the LCD.

---

## 6. Outstanding TODOs

1. **M7 waveform interactivity** — pan/zoom touch gestures and tap-on-lane toggles need event handlers (lane_mask and zoom factor exist).
2. **Legacy `c`/`C` opcodes** — 22-byte `configTable` schema emits zeros instead of the real packed config. Electron GUI reads defaults — not a full parity ship.
3. **M3.6 final legacy delete** — `TimerCkpGenerator` retained as `!SIGGEN_BACKEND_TABLE` fallback. Safe to remove once hardware sign-off confirms TableCkpGenerator parity AND WROOM build path is migrated.
4. **DSL `c` rotation semantics** — Agent D flagged ambiguity: `c` overloaded for both "720° period" and "reverse rotation". Asymmetric cam patterns may flip polarity post-canonicalization. Worked-example revision recommended.
5. **60-2 + half-moon cam DSL regression** — Agent D regression compiles to 240 bytes / 720° (correct dimensions) but does not byte-match `sixty_minus_two_with_halfmoon_cam` because the reference encodes cam transitions at specific teeth (44/43) not expressible via symmetric DSL. An angular-cam DSL form would match — left as worked-example revision.

### Resolved in Implementation-2 (2026-05-22)

- **Backend acknowledgement contract** — `IGenerator::start/stop` now return `bool`; `lastError()`/`isReady()` virtuals added. UI strings come from `genErrorString(GenError)` in `src/main.cpp`. Generic "Pattern apply failed" is gone.
- **Output pin validation** — `TableCkpGenerator::isValidEsp32S3OutputPin()` rejects any pin in the JC4827W543 reserved set or outside the S3 valid GPIO range, logging the offending pin to Serial. `PIN_CAM1_OUT=21` (LCD QSPI) and `PIN_CAM2_OUT=22` (invalid on S3) are correctly rejected at runtime. `gGen.begin()` is now invoked crank-only (`-1, -1` for cam) behind a `USER: SELECT BACKEND OUTPUT PINS` banner in `src/main.cpp` — pin macros themselves remain at file scope for one-line user edit.
- **ISR cache safety** — 24 KB internal-DRAM playback buffer allocated once at `begin()`; `apply()` `memcpy`s patterns into it; ISR reads only from DRAM. Buffer size is at `TableCkpGenerator::kPlaybackBufferBytes` and is clearly banner-commented at the allocation site for future tuning.
- **Main screen layout** — rebuilt on a 480×272 grid with explicit left-pane and right-pane containers; no rectangle intersections; INVERT/START on their own bottom row; SWEEP/COMP/DSL/WAVE in a 2×2 grid.
- **Display/touch rotation lock** — `pick_display_rotation()` auto-loop replaced with `kDisplayRotation = 1`; touch rotation paired and annotated; boot Serial prints rotation + width/height for ground-truth verification.
- **RPM arc throttle + NVS debounce** — `on_arc_changed` fires only on `LV_EVENT_RELEASED` or 50 ms coalesce; `NvsStore::setRpmDebounced()` records latest and `tickRpmDebounce()` commits after 750 ms of no further changes.
- **Build hygiene** — partition `littlefs` → `spiffs` (warning gone); `lvgl@9.2.2` and `GFX Library@1.6.4` pinned exact; new `boards/esp32-s3-n4r8.json` declares 4 MB flash + 8 MB OPI PSRAM; boot prints flash/PSRAM/heap diagnostics.

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
| **DRAM playback buffer (R1.3)** | — | **24,576 B fixed** | ✅ Allocated once at boot via `heap_caps_malloc(MALLOC_CAP_INTERNAL)`; tune at `TableCkpGenerator::kPlaybackBufferBytes` |
| **Full firmware image (post-R)** | 4 MB factory | 837,203 B (Flash 20.0%) | ✅ |
| **DIRAM / internal SRAM use (post-R)** | 320 KB usable | 159,635 B (46.7%) | ✅ Includes 24 KB playback buffer + LVGL draw buffer |

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

---

## 10. Implementation-2 Build Validation (2026-05-22)

After the A/C/E remediation pass driven by `_Plans-and-Records/implementation-2_plan.md`:

| Criterion | Result |
|---|---|
| `pio run -e esp32-s3-n4r8` clean | ✅ SUCCESS (125.71 s) |
| Partition warnings | ✅ None |
| Other compiler/linker warnings | ✅ None |
| LVGL resolved version | ✅ `lvgl @ 9.2.2` exact |
| GFX Library resolved version | ✅ `1.6.4` exact |
| Board metadata | ✅ Custom `esp32-s3-n4r8` (4 MB flash / 8 MB OPI PSRAM) |
| Flash usage | ✅ 837,203 B / 4,194,304 B (20.0%) |
| DIRAM usage | ✅ 159,635 B / 341,760 B (46.7%) |
| IRAM usage | 100% (16,384 B) — within budget; ISR + vectors only |

Next gate is bench verification per §5 (Implementation-2 bench gates).

---

## 11. Prior Snapshot Reference

The 2026-05-20 snapshot text is preserved in this file under §1's "Original (2026-05-20) summary:" paragraph and §6 "Resolved in cleanup pass (2026-05-20)". The milestone table in §2 retains the original M-series rows unchanged and adds an R-series for Implementation-2 remediation milestones.
