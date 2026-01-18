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

// Validate basic config sanity (does not clamp).
bool validateSignalConfig(const SignalConfig& cfg);

// Simple generator interface for modular use
struct IGenerator {
  virtual bool begin(int pin) = 0;
  virtual bool apply(const SignalConfig& cfg) = 0;
  virtual void start() = 0;
  virtual void stop() = 0;
  virtual ~IGenerator() = default;
};

// Slot-based CKP generator using an ESP32 hardware timer ISR
class TimerCkpGenerator : public IGenerator {
public:
  TimerCkpGenerator();

  bool begin(int pin) override;
  bool apply(const SignalConfig& cfg) override;
  void start() override;
  void stop() override;

  // Optional helpers for external monitoring/plotting
  bool     readCkpLevel();
  bool     readGapWindow();
  uint32_t getSlotPeriodUs();

private:
  inline void writePin(bool level);
  inline void writeHigh();
  inline void writeLow();
  static void ARDUINO_ISR_ATTR onTimerStatic();
  void onTimer();

  int         _pin;
  hw_timer_t* _timer;

  // Derived configuration
  volatile uint32_t   _slotPeriod_us;
  volatile uint32_t   _slotsPerRev;
  volatile uint32_t   _slotsPerPeriod;
  volatile uint32_t   _gapSlots;
  volatile GapPosition _gapPos;
  volatile bool        _gapLvl;

  // Precomputed ISR helpers
  volatile uint32_t _gapStartSip;

  // State
  volatile bool     _running;
  volatile bool     _pinHigh;
  volatile uint32_t _slotInPeriod;
  volatile bool     _gapWindow;


  portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
  static TimerCkpGenerator* s_inst;
};

