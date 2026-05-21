// TimerCkpGenerator — legacy slot-machine ISR backend, now implementing the
// widened IGenerator surface (implementation_plan.md §3.2, WP M0.1).
//
// What "synthesizing a byte table at runtime from a SignalConfig" means
// here: applySignalConfig() expands the symmetric/missing-tooth waveform
// into an internal uint8_t[slotsPerRev] buffer (one byte per slot, bit0 =
// crank level) for inspection / future migration. The ISR itself still
// computes the level on the fly from _slotInPeriod, _gapStartSip and
// _gapLvl — there is no buffer playback in this adapter. The real
// byte-walking ISR is delivered by TableCkpGenerator in WP M1.1.

#include "CkpGenerator.h"
#include <stdlib.h>
#include <string.h>

// Optional local debug (off by default)
#ifndef CKPGEN_DEBUG
#define CKPGEN_DEBUG 0
#endif
#if CKPGEN_DEBUG
  #define CKPDBG_PRINTLN(x) Serial.println(x)
  #define CKPDBG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
  #define CKPDBG_PRINTLN(x)
  #define CKPDBG_PRINTF(...)
#endif

TimerCkpGenerator* TimerCkpGenerator::s_inst = nullptr;

TimerCkpGenerator::TimerCkpGenerator()
  : _pin(-1), _pin_cam1(-1), _pin_cam2(-1), _timer(nullptr),
    _slotPeriod_us(500), _slotsPerRev(120), _slotsPerPeriod(120),
    _gapSlots(4), _gapPos(GAP_AT_END), _gapLvl(false),
    _gapStartSip(0), _invertMask(0),
    _running(false), _pinHigh(false),
    _slotInPeriod(0), _gapWindow(false),
    _edge_counter(0),
    _synth_table(nullptr), _synth_slot_count(0), _last_rpm(0) {}

TimerCkpGenerator::~TimerCkpGenerator() {
  if (_synth_table) {
    free(_synth_table);
    _synth_table = nullptr;
  }
}


bool TimerCkpGenerator::begin(int pin_crank, int pin_cam1, int pin_cam2) {
  _pin      = pin_crank;
  _pin_cam1 = pin_cam1;
  _pin_cam2 = pin_cam2;

  pinMode(_pin, OUTPUT);
  writeLow();

  // Cam pins are accepted for forward-compat with TableCkpGenerator but
  // the legacy slot machine drives only the crank pin. Park them as
  // INPUT (high-Z) so they do not float a previously-driven level.
  if (_pin_cam1 >= 0) pinMode(_pin_cam1, INPUT);
  if (_pin_cam2 >= 0) pinMode(_pin_cam2, INPUT);

  // 1 MHz base (1 tick = 1 us)
  _timer = timerBegin(1000000);
  if (!_timer) {
    CKPDBG_PRINTLN("[CKP] timerBegin() failed");
    return false;
  }

  timerAttachInterrupt(_timer, &TimerCkpGenerator::onTimerStatic);
  s_inst = this;
  return true;
}


bool validateSignalConfig(const SignalConfig& cfg) {
  if (cfg.rpm < 100u || cfg.rpm > 6000u) return false;
  if (cfg.nTeeth == 0) return false;
  if (cfg.pMiss == 0) return false;
  if (cfg.nMiss == 0) return false;
  if (cfg.nMiss >= cfg.nTeeth) return false;

  const uint32_t slotsPerRev = 2u * (uint32_t)cfg.nTeeth;
  const uint32_t slotsPerPeriod = (cfg.pMiss > 0) ? (slotsPerRev / cfg.pMiss) : 0;
  if (slotsPerPeriod == 0) return false;
  if ((slotsPerRev % cfg.pMiss) != 0) return false;

  const uint32_t gapSlots = 2u * (uint32_t)cfg.nMiss;
  if (gapSlots >= slotsPerPeriod) return false;

  return true;
}

// Synthesize a byte table from the active SignalConfig-derived state.
// One byte per slot in a full revolution; bit0 = crank pin level for
// that slot (BEFORE invert). Stored on heap; freed in the destructor or
// on re-synthesis. Used today only for inspection / future migration —
// the ISR continues to compute levels algorithmically.
static uint8_t* synthesize_table(uint32_t slotsPerRev,
                                 uint32_t slotsPerPeriod,
                                 uint32_t gapSlots,
                                 GapPosition gapPos,
                                 bool gapLvl,
                                 uint16_t* out_count) {
  if (slotsPerRev == 0 || slotsPerPeriod == 0) {
    if (out_count) *out_count = 0;
    return nullptr;
  }
  uint8_t* buf = (uint8_t*)malloc(slotsPerRev);
  if (!buf) {
    if (out_count) *out_count = 0;
    return nullptr;
  }
  const uint32_t gapStartSip = (gapPos == GAP_AT_END)
                                 ? (slotsPerPeriod - gapSlots) : 0u;
  for (uint32_t i = 0; i < slotsPerRev; ++i) {
    const uint32_t sip = i % slotsPerPeriod;
    bool level;
    bool in_gap;
    if (gapPos == GAP_AT_END) {
      in_gap = (sip >= gapStartSip);
      if (in_gap) level = gapLvl;
      else        level = ((sip & 1u) == (gapLvl ? 1u : 0u));
    } else { // GAP_AT_START
      in_gap = (sip < gapSlots);
      if (in_gap) level = gapLvl;
      else {
        const uint32_t tooth_sip = sip - gapSlots;
        level = (((tooth_sip & 1u) == 0u) != gapLvl);
      }
    }
    buf[i] = level ? 0x01u : 0x00u; // bit0 = crank
  }
  if (out_count) *out_count = (uint16_t)slotsPerRev;
  return buf;
}

bool TimerCkpGenerator::applySignalConfig(const SignalConfig& cfg) {
  if (!validateSignalConfig(cfg)) return false;

  uint32_t slotsPerRev    = 2u * cfg.nTeeth;
  uint32_t slotsPerPeriod = slotsPerRev / cfg.pMiss;
  uint32_t gapSlots       = 2u * cfg.nMiss;

  uint64_t denom   = (uint64_t)cfg.rpm * (uint64_t)slotsPerRev;
  uint32_t slot_us = (uint32_t)(60000000ULL / (denom ? denom : 1ULL));

  portENTER_CRITICAL(&_mux);
  _slotsPerRev    = slotsPerRev;
  _slotsPerPeriod = slotsPerPeriod ? slotsPerPeriod : 1;
  _gapSlots       = gapSlots;
  _slotPeriod_us  = slot_us ? slot_us : 1;
  _gapPos         = cfg.gapPos;
  _gapLvl         = cfg.gapLvl;
  _gapStartSip    = (_gapPos == GAP_AT_END) ? (_slotsPerPeriod - _gapSlots) : 0;
  _pinHigh = false;
  _slotInPeriod = 0;
  _edge_counter = 0;

  _gapWindow = false;
  _last_rpm  = cfg.rpm;
  writeLow();
  portEXIT_CRITICAL(&_mux);

  // Synthesize the inspection byte table outside the critical section.
  uint16_t new_count = 0;
  uint8_t* new_table = synthesize_table(slotsPerRev, _slotsPerPeriod,
                                        _gapSlots, _gapPos, _gapLvl,
                                        &new_count);
  if (_synth_table) free(_synth_table);
  _synth_table      = new_table;
  _synth_slot_count = new_count;

  if (_running && _timer) {
    timerAlarm(_timer, _slotPeriod_us, true, 0);
  }

  CKPDBG_PRINTF("[CKP] apply: rpm=%u, teeth=%u-%u, periods=%u, gapPos=%s, gapLvl=%s\n",
                cfg.rpm, cfg.nTeeth, cfg.nMiss, cfg.pMiss,
                _gapPos == GAP_AT_END ? "END" : "START",
                _gapLvl ? "HIGH" : "LOW");

  return true;
}

bool TimerCkpGenerator::apply(const PatternRef& ref, uint32_t rpm) {
  // Legacy adapter cannot replay arbitrary byte tables — that is the
  // job of TableCkpGenerator (WP M1.1). Compute timer parameters in case
  // a future caller wants a "metadata-only" apply, but return false to
  // signal that the underlying ISR is NOT driving the supplied table.
  //
  // Ardu-Stim timer formula (AVR): OCR1A = 8000000 / (rpm_scaler * rpm).
  // The ESP32 1 MHz hw_timer is 1 µs/tick, so the equivalent slot period
  // in microseconds is 1000000 / (rpm_scaler * rpm / 60) for a one-edge-
  // per-slot table, which simplifies to 60000000 / (slot_count * rpm)
  // using slot_count = rpm_scaler * 120.
  if (ref.table == nullptr || ref.slot_count == 0 || rpm == 0) return false;

  const uint64_t denom = (uint64_t)rpm * (uint64_t)ref.slot_count;
  const uint32_t slot_us = (uint32_t)(60000000ULL / (denom ? denom : 1ULL));

  portENTER_CRITICAL(&_mux);
  _slotPeriod_us = slot_us ? slot_us : 1;
  _last_rpm      = rpm;
  portEXIT_CRITICAL(&_mux);

  CKPDBG_PRINTF("[CKP] apply(PatternRef '%s', rpm=%u) — legacy adapter; "
                "byte table NOT driven (use TableCkpGenerator in M1.1)\n",
                ref.name_key ? ref.name_key : "?", rpm);
  return false;
}

bool TimerCkpGenerator::setRpm(uint32_t rpm) {
  // Fast path — never rebuilds buffers. Recomputes _slotPeriod_us from
  // the currently active _slotsPerRev (set by the most recent
  // applySignalConfig). The sweep task (WP M4.1) calls only this entry
  // point.
  if (rpm == 0) return false;
  if (rpm < 100u)  rpm = 100u;
  if (rpm > 20000u) rpm = 20000u; // wide upper clamp for sweep headroom

  uint32_t slotsPerRev;
  portENTER_CRITICAL(&_mux);
  slotsPerRev = _slotsPerRev;
  portEXIT_CRITICAL(&_mux);
  if (slotsPerRev == 0) return false;

  const uint64_t denom   = (uint64_t)rpm * (uint64_t)slotsPerRev;
  const uint32_t slot_us = (uint32_t)(60000000ULL / (denom ? denom : 1ULL));

  portENTER_CRITICAL(&_mux);
  _slotPeriod_us = slot_us ? slot_us : 1;
  _last_rpm      = rpm;
  portEXIT_CRITICAL(&_mux);

  if (_running && _timer) {
    timerAlarm(_timer, _slotPeriod_us, true, 0);
  }
  return true;
}

void TimerCkpGenerator::setInverted(uint8_t channel_mask) {
  // Legacy backend only drives the crank channel — bit0 is the only bit
  // that has visible effect. Higher bits are stored for round-trip
  // fidelity with getInverted() and will become live in M2.1.
  portENTER_CRITICAL(&_mux);
  const uint8_t prev = _invertMask;
  _invertMask = channel_mask;
  const bool prev_crank_inv = (prev & 0x01u) != 0;
  const bool new_crank_inv  = (channel_mask & 0x01u) != 0;
  if (prev_crank_inv != new_crank_inv && _running) {
    writePin(!_pinHigh);
  }
  portEXIT_CRITICAL(&_mux);
}

uint8_t TimerCkpGenerator::getInverted() const {
  // _invertMask is a single byte — aligned reads are atomic on Xtensa.
  // No critical section needed.
  return _invertMask;
}

void TimerCkpGenerator::start() {
  if (!_timer) return;
  portENTER_CRITICAL(&_mux);
  _pinHigh = false; _running = true;
  _slotInPeriod = 0; _gapWindow = false;
  _edge_counter = 0;
  writeLow();
  portEXIT_CRITICAL(&_mux);

  timerAlarm(_timer, _slotPeriod_us, true, 0);
  CKPDBG_PRINTLN("[CKP] generator START");
}

void TimerCkpGenerator::stop() {
  portENTER_CRITICAL(&_mux);
  _running = false; writeLow();
  portEXIT_CRITICAL(&_mux);
  CKPDBG_PRINTLN("[CKP] generator STOP");
}

uint16_t TimerCkpGenerator::getEdgeCounter() const {
  // Naturally aligned uint16_t — atomic read on Xtensa. No critical
  // section per the IGenerator contract (CkpGenerator.h invariants).
  return _edge_counter;
}

bool TimerCkpGenerator::readCkpLevel() {
  bool v; portENTER_CRITICAL(&_mux); v=_pinHigh;      portEXIT_CRITICAL(&_mux); return v;
}
bool TimerCkpGenerator::readGapWindow() {
  bool v; portENTER_CRITICAL(&_mux); v=_gapWindow;    portEXIT_CRITICAL(&_mux); return v;
}
uint32_t TimerCkpGenerator::getSlotPeriodUs() {
  uint32_t v; portENTER_CRITICAL(&_mux); v=_slotPeriod_us; portEXIT_CRITICAL(&_mux); return v;
}

inline void TimerCkpGenerator::writePin(bool level) {
  REG_WRITE(level ? GPIO_OUT_W1TS_REG : GPIO_OUT_W1TC_REG, (1U << _pin));
  _pinHigh = level;
}
inline void TimerCkpGenerator::writeHigh() { writePin(true); }
inline void TimerCkpGenerator::writeLow()  { writePin(false); }

void ARDUINO_ISR_ATTR TimerCkpGenerator::onTimerStatic() {
  if (s_inst) s_inst->onTimer();
}

void ARDUINO_ISR_ATTR TimerCkpGenerator::onTimer() {
  portENTER_CRITICAL_ISR(&_mux);
  if (!_running) { portEXIT_CRITICAL_ISR(&_mux); return; }

  const uint32_t sip = _slotInPeriod;
  uint32_t nextSip = sip + 1;
  if (nextSip >= _slotsPerPeriod) nextSip = 0;
  _slotInPeriod = nextSip;

  bool level;
  bool in_gap;

  if (_gapPos == GAP_AT_END) {
    in_gap = (sip >= _gapStartSip);
    if (in_gap) {
      level = _gapLvl;
    } else {
      level = ((sip & 1u) == (_gapLvl ? 1u : 0u));
    }
  } else { // GAP_AT_START
    in_gap = (sip < _gapSlots);
    if (in_gap) {
      level = _gapLvl;
    } else {
      const uint32_t tooth_sip = sip - _gapSlots;
      level = (((tooth_sip & 1u) == 0u) != _gapLvl);
    }
  }

  // Per-channel invert mask — bit0 is the crank pin. Higher bits are
  // honored by TableCkpGenerator in M2.1; here they are stored only.
  if (_invertMask & 0x01u) {
    level = !level;
  }

  if (level != _pinHigh) {
    writePin(level);
  }
  _gapWindow = in_gap;

  // Advance the edge counter. Aligned uint16_t — readers are lock-free.
  uint16_t ec = _edge_counter + 1u;
  if (_slotsPerRev != 0 && ec >= (uint16_t)_slotsPerRev) ec = 0;
  _edge_counter = ec;

  portEXIT_CRITICAL_ISR(&_mux);
}
