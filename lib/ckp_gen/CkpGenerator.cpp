#include "CkpGenerator.h"

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
  : _pin(-1), _timer(nullptr),
    _slotPeriod_us(500), _slotsPerRev(120), _slotsPerPeriod(120),
    _gapSlots(4), _gapPos(GAP_AT_END), _gapLvl(false),
    _gapStartSip(0), _invertOutput(false),
    _running(false), _pinHigh(false),
    _slotInPeriod(0), _gapWindow(false) {}


bool TimerCkpGenerator::begin(int pin) {
  _pin = pin;
  pinMode(_pin, OUTPUT);
  writeLow();

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

bool TimerCkpGenerator::apply(const SignalConfig& cfg) {
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

  return true;
}

void TimerCkpGenerator::setInverted(bool inverted) {
  portENTER_CRITICAL(&_mux);
  if (_invertOutput != inverted) {
    _invertOutput = inverted;
    if (_running) {
      writePin(!_pinHigh);
    }
  }
  portEXIT_CRITICAL(&_mux);
}

bool TimerCkpGenerator::isInverted() {
  bool v;
  portENTER_CRITICAL(&_mux);
  v = _invertOutput;
  portEXIT_CRITICAL(&_mux);
  return v;
}

void TimerCkpGenerator::start() {
  if (!_timer) return;
  portENTER_CRITICAL(&_mux);
  _pinHigh = false; _running = true;
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

void TimerCkpGenerator::onTimer() {
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

  if (_invertOutput) {
    level = !level;
  }

  if (level != _pinHigh) {
    writePin(level);
  }
  _gapWindow = in_gap;

  portEXIT_CRITICAL_ISR(&_mux);
}

