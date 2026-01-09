#include "CkpGenerator.h"

// Optional local debug (off by default)
// '#ifndef' means "if not defined" at preprocessing time; we set a default.
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

// Definition of the static class data member declared in the header.
// Outside the class, we must qualify with 'ClassName::'. Initialized to null.
TimerCkpGenerator* TimerCkpGenerator::s_inst = nullptr;

void TimerCkpGenerator::begin(int pin) {
  _pin = pin;
  pinMode(_pin, OUTPUT);
  writeLow();

  // 1 MHz base (1 tick = 1 us)
  _timer = timerBegin(1000000);
  // Attach a static ISR function. '&Class::func' takes the function pointer
  // to the static member; it will forward to this instance via 's_inst'.
  timerAttachInterrupt(_timer, &TimerCkpGenerator::onTimerStatic);
  s_inst = this;
}

void TimerCkpGenerator::apply(const SignalConfig& cfg) {
  if (cfg.nTeeth == 0 || cfg.rpm == 0 || cfg.pMiss == 0) return;

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
  _tick = 0;
  _pinHigh = false;
  _slotInPeriod = 0;
  _gapWindow = false;
  writeLow();
  portEXIT_CRITICAL(&_mux);

  if (_running && _timer) {
    timerAlarm(_timer, _slotPeriod_us, true, 0);
  }

  CKPDBG_PRINTF("[CKP] apply: rpm=%u, teeth=%u-%u, periods=%u, gapPos=%s, gapLvl=%s\n",
                cfg.rpm, cfg.nTeeth, cfg.nMiss, cfg.pMiss,
                _gapPos == GAP_AT_END ? "END" : "START",
                _gapLvl ? "HIGH" : "LOW");
}

void TimerCkpGenerator::start() {
  if (!_timer) return;
  portENTER_CRITICAL(&_mux);
  _tick = 0; _pinHigh = false; _running = true;
  _slotInPeriod = 0; _gapWindow = false;
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

void TimerCkpGenerator::onTimer() {
  portENTER_CRITICAL_ISR(&_mux);
  if (!_running) { portEXIT_CRITICAL_ISR(&_mux); return; }

  if (_tick == _slotsPerRev-1) _tick = 0;

  uint32_t sip = _tick % _slotsPerPeriod;
  _slotInPeriod = sip;

  bool level;
  bool in_gap;

  if (_gapPos == GAP_AT_END) {
    in_gap = (sip >= (_slotsPerPeriod - _gapSlots));
    if (in_gap) {
      level = _gapLvl;
    } else {
      if(!_gapLvl){
        level = ((sip & 1u) == 0u);
      } else {
        level = ((sip & 1u) == 1u);
      }
    }
  } else { // GAP_AT_START
    in_gap = (sip < _gapSlots);
    if (in_gap) {
      level = _gapLvl;
    } else {
      uint32_t tooth_sip = sip - _gapSlots;
      level = (((tooth_sip & 1u) == 0u) != _gapLvl);
    }
  }

  if (level != _pinHigh) {
    writePin(level);
  }
  _gapWindow = in_gap;
  _tick = _tick + 1;
  portEXIT_CRITICAL_ISR(&_mux);
}
