// lib/ui_lvgl/serial_cli.cpp — see serial_cli.h.
//
// Detection rule (per §6 Agent E hard rules):
//   * If the next byte is alphabetic AND the byte after a contiguous
//     identifier is space/CR/LF, we are in TEXT mode for that line.
//   * Otherwise the byte is treated as a legacy single-byte opcode
//     matching References/comms.cpp.
//
// The legacy opcode set is implemented verbatim from comms.cpp, modulo
// the AVR-specific `pgm_read_byte`/PROGMEM bits (ESP32 access is direct).
// The `r` (set sweep range) command uses `word(hi, lo)` byte order — see
// References/comms.cpp:128-130 — meaning the HIGH byte arrives first on
// the wire. This is preserved here for Ardu-Stim Electron GUI parity.

#include "serial_cli.h"

#include <Arduino.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "PatternLibrary.h"
#include "NvsStore.h"
#include "PatternStorage.h"
#include "ctrl_msg.h"

static inline bool enq(MsgType type, int32_t v) {
  if (!gCtrlQ) return false;
  CtrlMsg m{};
  m.type = type;
  m.payload.val = v;
  return xQueueSend(gCtrlQ, &m, 0) == pdTRUE;
}

static inline bool enqName(MsgType type, const char* name_heap) {
  if (!gCtrlQ) return false;
  CtrlMsg m{};
  m.type = type;
  m.payload.name = name_heap;
  return xQueueSend(gCtrlQ, &m, 0) == pdTRUE;
}

static inline bool enqSweep(uint16_t lo, uint16_t hi, uint8_t mode, uint32_t iv) {
  if (!gCtrlQ) return false;
  CtrlMsg m{};
  m.type = MSG_SET_SWEEP;
  m.payload.sweep.low_rpm     = lo;
  m.payload.sweep.high_rpm    = hi;
  m.payload.sweep.mode        = mode;
  m.payload.sweep.interval_us = iv;
  return xQueueSend(gCtrlQ, &m, 0) == pdTRUE;
}

static inline bool enqComp(bool on, uint8_t cyl, uint16_t thr, uint8_t peak, bool dyn) {
  if (!gCtrlQ) return false;
  CtrlMsg m{};
  m.type = MSG_SET_COMPRESSION;
  m.payload.comp.enabled    = on;
  m.payload.comp.cyl        = cyl;
  m.payload.comp.rpm_thresh = thr;
  m.payload.comp.peak       = peak;
  m.payload.comp.dynamic    = dyn;
  return xQueueSend(gCtrlQ, &m, 0) == pdTRUE;
}

// --- Legacy state mirror -----------------------------------------------
//
// References/comms.cpp uses a global `config` blob; for ESP32 we track
// the current pattern via NVS globals (g_pattern_key) + PatternLibrary
// legacy-index → builtin lookups. The `L`, `n`, `N`, `p`, `P`, `S`
// opcodes are implemented against the legacy-index space (the same
// Wheels[] order Ardu-Stim Electron expects).

extern char     g_pattern_key[];
extern uint32_t g_rpm;

static int s_current_legacy_idx = 0;

static void emit_pattern_list() {
  // Send one wheel name per line — matches `L` semantics. Use friendly
  // names if available; otherwise fall back to the name_key. Order MUST
  // be the legacy Wheels[] order so existing Ardu-Stim Electron GUI can
  // index by `S`/`p`/`P` afterwards.
  const size_t n = PatternLibrary::builtinCount();
  for (size_t i = 0; i < n; ++i) {
    const PatternRef* p = PatternLibrary::findByLegacyIndex(i);
    if (!p) { Serial.println(""); continue; }
    const char* friendly = PatternLibrary::friendlyName(p->name_key);
    Serial.println(friendly ? friendly : p->name_key);
  }
}

// --- Line buffer for text mode -----------------------------------------

static char  s_line[256];
static size_t s_line_len = 0;
static bool  s_in_text_mode = false;   // sticky for current line
static bool  s_text_mode_decided = false;

static void reset_line() {
  s_line_len = 0;
  s_line[0] = '\0';
  s_in_text_mode = false;
  s_text_mode_decided = false;
}

// Parse a SELECT/LIST/etc text command.
static void dispatch_text(const char* line) {
  // Skip leading whitespace
  while (*line == ' ' || *line == '\t') ++line;
  if (*line == '\0') return;

  // Tokenize first word
  char cmd[16] = {0};
  size_t i = 0;
  while (line[i] && line[i] != ' ' && i < sizeof(cmd) - 1) {
    cmd[i] = (char)toupper((unsigned char)line[i]);
    ++i;
  }
  cmd[i] = '\0';
  const char* args = line + i;
  while (*args == ' ' || *args == '\t') ++args;

  if (strcmp(cmd, "LIST") == 0) {
    emit_pattern_list();
    return;
  }
  if (strcmp(cmd, "SELECT") == 0) {
    // SELECT <key>  -> MSG_SELECT_NAMED. Key must be string literal in our
    // pattern library (.rodata). We look it up to obtain the .rodata key
    // pointer; safer than passing a heap copy that we'd need to track.
    const PatternRef* p = PatternLibrary::findByKey(args);
    if (!p) {
      Serial.printf("ERR unknown key %s\n", args);
      return;
    }
    // We can pass p->name_key (which is .rodata) safely without free.
    CtrlMsg m{};
    m.type = MSG_SELECT_NAMED;
    m.payload.name = p->name_key;
    if (!gCtrlQ || xQueueSend(gCtrlQ, &m, 0) != pdTRUE) {
      Serial.println("ERR queue full");
    } else {
      Serial.println("OK");
    }
    return;
  }
  if (strcmp(cmd, "COMPILE") == 0) {
    // The DSL source may contain spaces. Heap-copy and hand off to manager.
    char* src = (char*)malloc(strlen(args) + 1);
    if (!src) { Serial.println("ERR oom"); return; }
    strcpy(src, args);
    if (!enqName(MSG_LOAD_DSL, src)) { free(src); Serial.println("ERR queue full"); }
    else Serial.println("OK");
    return;
  }
  if (strcmp(cmd, "SAVE") == 0) {
    // SAVE user/<name>
    // Serial CLI has no DSL source on hand — pass null for dsl_source so
    // the manager either reuses a previously-active DSL or falls back to
    // a stub alias (see MSG_SAVE_USER handler in main.cpp).
    const char* name = args;
    if (strncmp(name, "user/", 5) == 0) name += 5;
    char* heap = (char*)malloc(strlen(name) + 1);
    if (!heap) { Serial.println("ERR oom"); return; }
    strcpy(heap, name);
    if (!gCtrlQ) { free(heap); Serial.println("ERR queue full"); return; }
    CtrlMsg m{};
    m.type = MSG_SAVE_USER;
    m.payload.save.name       = heap;
    m.payload.save.dsl_source = nullptr;
    if (xQueueSend(gCtrlQ, &m, 0) != pdTRUE) {
      free(heap);
      Serial.println("ERR queue full");
    } else {
      Serial.println("OK");
    }
    return;
  }
  if (strcmp(cmd, "LOAD") == 0) {
    const char* name = args;
    if (strncmp(name, "user/", 5) == 0) name += 5;
    char buf[2048];
    if (!PatternStorage::loadDsl(name, buf, sizeof(buf))) {
      Serial.printf("ERR no such pattern %s\n", name);
      return;
    }
    char* src = (char*)malloc(strlen(buf) + 1);
    if (!src) { Serial.println("ERR oom"); return; }
    strcpy(src, buf);
    if (!enqName(MSG_LOAD_DSL, src)) { free(src); Serial.println("ERR queue full"); }
    else Serial.println("OK");
    return;
  }
  if (strcmp(cmd, "DELETE") == 0) {
    const char* name = args;
    if (strncmp(name, "user/", 5) == 0) name += 5;
    Serial.println(PatternStorage::deletePattern(name) ? "OK" : "ERR not found");
    return;
  }
  if (strcmp(cmd, "CAPTURE") == 0) {
    char sub[8] = {0};
    sscanf(args, "%7s", sub);
    for (char* q = sub; *q; ++q) *q = (char)toupper((unsigned char)*q);
    if (strcmp(sub, "START") == 0) {
      enq(MSG_CAPTURE_START, 2);
      Serial.println("OK");
    } else if (strcmp(sub, "STOP") == 0) {
      enq(MSG_CAPTURE_STOP, 0);
      Serial.println("OK");
    } else {
      Serial.println("ERR usage: CAPTURE START|STOP");
    }
    return;
  }
  if (strcmp(cmd, "SWEEP") == 0) {
    unsigned lo = 0, hi = 0, mode = 0; unsigned long iv = 0;
    if (sscanf(args, "%u %u %u %lu", &lo, &hi, &mode, &iv) != 4) {
      Serial.println("ERR usage: SWEEP <low> <high> <mode> <interval_us>");
      return;
    }
    enqSweep((uint16_t)lo, (uint16_t)hi, (uint8_t)mode, (uint32_t)iv);
    Serial.println("OK");
    return;
  }
  if (strcmp(cmd, "COMP") == 0) {
    char on_off[8] = {0};
    unsigned cyl = 0, thr = 0, peak = 0, dyn = 0;
    if (sscanf(args, "%7s %u %u %u %u", on_off, &cyl, &thr, &peak, &dyn) != 5) {
      Serial.println("ERR usage: COMP <on|off> <cyl> <thresh> <peak> <dyn>");
      return;
    }
    const bool on = (strcasecmp(on_off, "on") == 0) || on_off[0] == '1';
    enqComp(on, (uint8_t)cyl, (uint16_t)thr, (uint8_t)peak, dyn != 0);
    Serial.println("OK");
    return;
  }
  if (strcmp(cmd, "RPM") == 0) {
    unsigned r = 0;
    if (sscanf(args, "%u", &r) != 1) { Serial.println("ERR usage: RPM <value>"); return; }
    enq(MSG_SET_RPM, (int32_t)r);
    Serial.println("OK");
    return;
  }
  Serial.printf("ERR unknown command %s\n", cmd);
}

// --- Legacy single-byte opcodes (References/comms.cpp) ------------------

static void legacy_opcode(uint8_t op) {
  switch (op) {
    case 'a':
      // Ardu-Stim 'a' is a no-op in comms.cpp.
      break;

    case 'L': {
      // Send wheel names, one per line.
      emit_pattern_list();
      break;
    }

    case 'n':
      Serial.println((int)PatternLibrary::builtinCount());
      break;

    case 'N':
      Serial.println(s_current_legacy_idx);
      break;

    case 'p': {
      const PatternRef* p = PatternLibrary::findByLegacyIndex(s_current_legacy_idx);
      Serial.println(p ? (int)p->slot_count : 0);
      break;
    }

    case 'P': {
      const PatternRef* p = PatternLibrary::findByLegacyIndex(s_current_legacy_idx);
      if (!p) { Serial.println(""); Serial.println(0); break; }
      for (uint16_t x = 0; x < p->slot_count; ++x) {
        if (x) Serial.print(',');
        Serial.print((int)p->table[x]);
      }
      Serial.println("");
      Serial.println((int)p->degrees);
      break;
    }

    case 'R':
      Serial.println((unsigned long)g_rpm);
      break;

    case 'r': {
      // Sweep set — 6 bytes, HI-LO per pair per References/comms.cpp:128-130
      //   sweep_low_rpm  = word(hi, lo)   ; HI byte first on the wire
      //   sweep_high_rpm = word(hi, lo)
      //   sweep_interval = word(hi, lo)
      // Block until all 6 bytes arrive.
      uint32_t start = millis();
      while (Serial.available() < 6) {
        if (millis() - start > 2000) return;  // 2s safety timeout
        delay(1);
      }
      uint8_t b[6];
      for (int i = 0; i < 6; ++i) b[i] = (uint8_t)Serial.read();
      const uint16_t lo  = (uint16_t)((b[0] << 8) | b[1]);  // HI-LO
      const uint16_t hi  = (uint16_t)((b[2] << 8) | b[3]);
      const uint16_t iv  = (uint16_t)((b[4] << 8) | b[5]);
      // Switch to LINEAR sweep mode (matches Ardu-Stim's LINEAR_SWEPT_RPM).
      enqSweep(lo, hi, /*mode=*/1, /*interval_us=*/(uint32_t)iv);
      break;
    }

    case 's':
      // 'save current config' — our setters commit eagerly, but support
      // an explicit save for symmetry.
      NvsStore::saveAllFromGlobals();
      break;

    case 'S': {
      // Wait for the wheel-index byte.
      uint32_t start = millis();
      while (Serial.available() < 1) {
        if (millis() - start > 2000) return;
        delay(1);
      }
      const uint8_t tmp_wheel = (uint8_t)Serial.read();
      if (tmp_wheel < PatternLibrary::builtinCount()) {
        s_current_legacy_idx = tmp_wheel;
        const PatternRef* p = PatternLibrary::findByLegacyIndex(tmp_wheel);
        if (p) {
          CtrlMsg m{};
          m.type = MSG_SELECT_NAMED;
          m.payload.name = p->name_key;
          if (gCtrlQ) (void)xQueueSend(gCtrlQ, &m, 0);
        }
      }
      break;
    }

    case 'X': {
      // Cycle to next wheel.
      s_current_legacy_idx = (s_current_legacy_idx + 1) % (int)PatternLibrary::builtinCount();
      const PatternRef* p = PatternLibrary::findByLegacyIndex(s_current_legacy_idx);
      if (p) {
        CtrlMsg m{};
        m.type = MSG_SELECT_NAMED;
        m.payload.name = p->name_key;
        if (gCtrlQ) (void)xQueueSend(gCtrlQ, &m, 0);
        const char* friendly = PatternLibrary::friendlyName(p->name_key);
        Serial.println(friendly ? friendly : p->name_key);
      }
      break;
    }

    case 'c':
    case 'C':
      // configTable v2 — schema migration not fully implemented here.
      // TODO(M9.1): Honor the 22-byte configTable shape from
      // References/globals.h:40-56; for now we acknowledge the opcode and
      // skip / emit zeros so the Electron GUI's handshake does not stall.
      if (op == 'C') {
        // Send 22 zero bytes as a placeholder configTable.
        for (int i = 0; i < 22; ++i) Serial.write((uint8_t)0);
      } else {
        // Read 21 bytes (sizeof - 1; see comms.cpp:69) into a sink.
        uint32_t start = millis();
        while (Serial.available() < 21) {
          if (millis() - start > 2000) return;
          delay(1);
        }
        for (int i = 0; i < 21; ++i) (void)Serial.read();
      }
      break;

    default:
      break;
  }
}

// --- Polling loop ------------------------------------------------------

void serialCliBegin() {
  reset_line();
}

int serialCliPoll() {
  if (!Serial) return 0;

  int handled = 0;
  while (Serial.available() > 0) {
    if (!s_text_mode_decided && s_line_len == 0) {
      // Peek BEFORE consuming. The detection rule (per §6 Agent E hard
      // rules) is "first byte is alphabetic AND the byte after a
      // contiguous identifier is space => text mode; else legacy
      // single-byte". We implement that by peeking the next-but-one
      // byte once we've buffered alphabetic characters.
      const int p = Serial.peek();
      if (p < 0) break;
      const uint8_t c = (uint8_t)p;
      if (!isalpha(c)) {
        // Definitely legacy. legacy_opcode will Serial.read() the byte
        // itself (and any payload bytes); do NOT consume here.
        (void)Serial.read();
        legacy_opcode(c);
        ++handled;
        continue;
      }
      // Could be text. Consume the alpha char into buffer.
      (void)Serial.read();
      s_line[s_line_len++] = (char)c;
      continue;
    }

    if (!s_text_mode_decided) {
      // We've buffered ≥1 alpha char. Peek the next byte to decide.
      const int p = Serial.peek();
      if (p < 0) break;  // wait for more data
      const uint8_t c = (uint8_t)p;
      if (isalpha(c) || isdigit(c) || c == '_' || c == '-') {
        (void)Serial.read();
        if (s_line_len < sizeof(s_line) - 1) s_line[s_line_len++] = (char)c;
        continue;
      }
      if (c == ' ' || c == '\t') {
        // It IS text mode.
        (void)Serial.read();
        s_text_mode_decided = true;
        s_in_text_mode = true;
        if (s_line_len < sizeof(s_line) - 1) s_line[s_line_len++] = (char)c;
        continue;
      }
      if (c == '\r' || c == '\n') {
        // Could be a single-word text command ("LIST\n") OR a legacy
        // opcode followed by a CR/LF. Heuristic: if our buffered text
        // matches a known text command (LIST, RPM, etc.), dispatch as
        // text; otherwise dispatch each buffered byte as legacy opcodes.
        s_line[s_line_len] = '\0';
        // Uppercase for compare
        char upper[sizeof(s_line)];
        for (size_t i = 0; i < s_line_len; ++i) {
          upper[i] = (char)toupper((unsigned char)s_line[i]);
        }
        upper[s_line_len] = '\0';
        const bool text_known =
          strcmp(upper, "LIST") == 0 ||
          strcmp(upper, "SELECT") == 0 ||
          strcmp(upper, "COMPILE") == 0 ||
          strcmp(upper, "SAVE") == 0 ||
          strcmp(upper, "LOAD") == 0 ||
          strcmp(upper, "DELETE") == 0 ||
          strcmp(upper, "CAPTURE") == 0 ||
          strcmp(upper, "SWEEP") == 0 ||
          strcmp(upper, "COMP") == 0 ||
          strcmp(upper, "RPM") == 0;
        if (text_known) {
          (void)Serial.read();  // consume CR/LF
          dispatch_text(s_line);
          ++handled;
          reset_line();
        } else {
          // Dispatch each buffered char as legacy opcode (single chars).
          for (size_t i = 0; i < s_line_len; ++i) {
            legacy_opcode((uint8_t)s_line[i]);
            ++handled;
          }
          reset_line();
          // Leave the CR/LF in the stream — next iteration will treat
          // it as a noop (isalpha=false; legacy_opcode default case).
        }
        continue;
      }
      // Anything else (likely a binary payload byte following a legacy
      // opcode). Dispatch buffered chars as legacy and LEAVE `c` in the
      // stream so the legacy handler can re-read it.
      for (size_t i = 0; i < s_line_len; ++i) {
        legacy_opcode((uint8_t)s_line[i]);
        ++handled;
      }
      reset_line();
      continue;
    }

    // In text mode: accumulate until CR/LF.
    const int ci = Serial.read();
    if (ci < 0) break;
    const uint8_t c = (uint8_t)ci;
    if (c == '\r') continue;
    if (c == '\n') {
      s_line[s_line_len] = '\0';
      dispatch_text(s_line);
      ++handled;
      reset_line();
      continue;
    }
    if (s_line_len < sizeof(s_line) - 1) s_line[s_line_len++] = (char)c;
  }
  return handled;
}
