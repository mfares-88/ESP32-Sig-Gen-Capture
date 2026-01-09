// Minimal, modular signal generation API for ESP32 CKP output
#pragma once

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "soc/gpio_reg.h"
#include "driver/gpio.h"
#include "esp32-hal-timer.h"

// Position of the missing-tooth gap within a period
enum GapPosition {
  GAP_AT_END,
  GAP_AT_START
};

// Configuration for the generated crank signal
struct SignalConfig {
  uint32_t   rpm;        // 100..6000
  uint16_t   nTeeth;     // total teeth if no gaps (e.g., 60, 36)
  uint8_t    pMiss;      // number of gap periods per revolution
  uint8_t    nMiss;      // number of missing teeth per period
  GapPosition gapPos;    // START or END of period
  bool        gapLvl;    // gap level (false=LOW, true=HIGH)
};

// Simple generator interface for modular use
struct IGenerator {
  virtual void begin(int pin) = 0;
  virtual void apply(const SignalConfig& cfg) = 0;
  virtual void start() = 0;
  virtual void stop() = 0;
  virtual ~IGenerator() = default;
};

// Slot-based CKP generator using an ESP32 hardware timer ISR
class TimerCkpGenerator : public IGenerator {
public:
  // Constructor defined inside the class body:
  // - In-class definitions are implicitly 'inline' (may appear in multiple
  //   translation units safely) and avoid repeating 'ClassName::' elsewhere.
  // - Member initializer list sets fields before the body runs.
  TimerCkpGenerator()
    : _pin(-1), _timer(nullptr),
      _slotPeriod_us(500), _slotsPerRev(120), _slotsPerPeriod(120),
      _gapSlots(4), _gapPos(GAP_AT_END), _gapLvl(false),
      _tick(0), _running(false), _pinHigh(false),
      _slotInPeriod(0), _gapWindow(false) {}

  void begin(int pin) override;
  void apply(const SignalConfig& cfg) override;
  void start() override;
  void stop() override;

  // Optional helpers for external monitoring/plotting.
  // Defined in-class to be small, readable, and implicitly inline.
  bool readCkpLevel() {
    bool v; portENTER_CRITICAL(&_mux); v = _pinHigh;      portEXIT_CRITICAL(&_mux); return v;
  }
  bool readGapWindow() {
    bool v; portENTER_CRITICAL(&_mux); v = _gapWindow;    portEXIT_CRITICAL(&_mux); return v;
  }
  uint32_t getSlotPeriodUs() {
    uint32_t v; portENTER_CRITICAL(&_mux); v = _slotPeriod_us; portEXIT_CRITICAL(&_mux); return v;
  }

private:
  // Tiny pin write helpers defined in-class (implicitly inline) for speed.
  // Uses a register write to set/clear the GPIO output atomically.
  inline void writePin(bool level) {
    REG_WRITE(level ? GPIO_OUT_W1TS_REG : GPIO_OUT_W1TC_REG, (1U << _pin));
    _pinHigh = level; // keep software state in sync with the pin
  }
  inline void writeHigh() { writePin(true); }
  inline void writeLow()  { writePin(false); }

  // Static ISR trampoline: required because many timer APIs need a C/static
  // function pointer. It forwards to the current instance stored in 's_inst'.
  static void ARDUINO_ISR_ATTR onTimerStatic() { if (s_inst) s_inst->onTimer(); }
  void onTimer();

  int         _pin;
  hw_timer_t* _timer;

  // Derived configuration
  // 'volatile' because these fields are accessed/modified in both task and ISR
  // contexts; it prevents the compiler from optimizing away necessary reloads.
  volatile uint32_t   _slotPeriod_us;
  volatile uint32_t   _slotsPerRev;
  volatile uint32_t   _slotsPerPeriod;
  volatile uint32_t   _gapSlots;
  volatile GapPosition _gapPos;
  volatile bool        _gapLvl;

  // State
  volatile uint32_t _tick;
  volatile bool     _running;
  volatile bool     _pinHigh;
  volatile uint32_t _slotInPeriod;
  volatile bool     _gapWindow;

  portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED; // FreeRTOS spinlock for critical sections
  static TimerCkpGenerator* s_inst; // declaration of a static data member (defined once in .cpp)
};
