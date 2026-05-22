// Signal generation API for ESP32 CKP / CAM outputs.
//
// M0.1 (interface freeze): IGenerator widened per implementation_plan.md §3.2
// to accept a PatternRef + RPM, a fast setRpm() path, a per-channel invert
// mask, and an atomic edge-counter accessor. `SignalConfig` is preserved
// here because the existing "Symmetric/Missing" UI modal still feeds it
// directly; from M5 onward this path will be routed through the DSL
// compiler (dslCompileSignalConfig).
//
// The legacy TimerCkpGenerator class continues to drive the single crank
// pin via its slot-machine timer ISR, but now implements the wider
// IGenerator surface. Its `apply(const PatternRef&, rpm)` path is
// best-effort (no byte-table playback in the legacy backend); the real
// byte-table backend lands in M1.1 as TableCkpGenerator. main.cpp talks
// to the legacy adapter through `applySignalConfig()` until Agent E
// rewires it in M0.2.

#pragma once

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "soc/gpio_reg.h"
#include "driver/gpio.h"
#include "esp32-hal-timer.h"

#include "PatternRef.h"

enum class GenError : uint8_t {
  OK = 0,
  NOT_INITIALIZED,
  NO_TABLE,
  BAD_SLOT_COUNT,
  BAD_RPM,
  TIMER_FAIL,
  GPIO_FAIL,
  BUFFER_OVERFLOW
};

// Position of the missing-tooth gap within a period
enum GapPosition {
  GAP_AT_END,
  GAP_AT_START
};

// Configuration for the generated crank signal (legacy "Symmetric/Missing"
// modal input). Retained through M4 per §8 risk register item #7; removed
// when M5.7 ships the full DSL editor.
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

// IGenerator — manager <-> backend contract (implementation_plan.md §3.2).
//
// Invariants:
//   - getEdgeCounter() returns a single naturally-aligned uint16_t and is
//     read without a critical section (Xtensa aligned 16-bit reads are
//     atomic against the ISR writer of the same word).
//   - setRpm() never reallocates or rebuilds buffers — it is the fast path
//     used by the sweep task at priority 2.
//   - apply() may rebuild buffers; callers must not invoke from ISR context.
//   - channel_mask bit positions: bit0=crank, bit1=cam1, bit2=cam2,
//     bit3=knock(reserved). The mask XORs the published byte before
//     bundle write — i.e. a set bit means "invert this channel".
struct IGenerator {
  virtual bool begin(int pin_crank, int pin_cam1 = -1, int pin_cam2 = -1) = 0;
  virtual bool apply(const PatternRef& ref, uint32_t rpm) = 0;
  virtual bool setRpm(uint32_t rpm) = 0;              // fast path — used in sweep
  virtual void setInverted(uint8_t channel_mask) = 0; // per-channel XOR
  virtual uint8_t getInverted() const = 0;
  virtual bool start() = 0;
  virtual bool stop() = 0;
  virtual uint16_t getEdgeCounter() const = 0;        // atomic read for waveform cursor

  virtual GenError lastError() const { return GenError::OK; }
  virtual bool isReady() const { return false; }

  // Cycle-timing accessors (M4.2, Agent C consumer). Published by the byte-
  // table backend on every wrap of the active pattern — the compression
  // task uses (now - cycleStart) / cycleDuration to derive the current
  // crank angle. Naturally aligned 32-bit reads on Xtensa are atomic
  // against the ISR writer; no critical section required.
  //
  // Default implementations return 0 so the legacy adapter (which does not
  // track cycle timing) silently disables the compression effect — the
  // calculateCompressionModifier() consumer treats cycleDuration == 0 as
  // "no signal", matching References/ardustim.ino:379. This is a minor
  // interface widening introduced when M4.2 needed the crank-angle hook.
  virtual uint32_t getCycleStartUs() const { return 0; }
  virtual uint32_t getCycleDurationUs() const { return 0; }

  virtual ~IGenerator() = default;
};

// Legacy slot-machine CKP generator using an ESP32 hardware timer ISR.
//
// Implements the new IGenerator surface (M0.1) while internally still
// running the original symmetric/missing-tooth slot machine on the crank
// pin only. cam1/cam2 pins are accepted by begin() but parked as INPUT
// (no output). apply(PatternRef, rpm) is provided for API completeness
// but cannot replay arbitrary byte tables — it returns false unless the
// caller is using the SignalConfig path via applySignalConfig().
//
// DEPRECATED — to be deleted after Agent E completes M1.3 integration.
// Use TableCkpGenerator (TableCkpGenerator.h) for all new code paths.
class TimerCkpGenerator : public IGenerator {
public:
  TimerCkpGenerator();

  // IGenerator (new surface)
  bool begin(int pin_crank, int pin_cam1 = -1, int pin_cam2 = -1) override;
  bool apply(const PatternRef& ref, uint32_t rpm) override;
  bool setRpm(uint32_t rpm) override;
  void setInverted(uint8_t channel_mask) override;
  uint8_t getInverted() const override;
  bool start() override;
  bool stop() override;
  uint16_t getEdgeCounter() const override;

  // Backward-compatible SignalConfig entry point. main.cpp continues to
  // call this through M0.1/M0.2; from M5 the UI modal routes through the
  // DSL compiler instead.
  bool applySignalConfig(const SignalConfig& cfg);

  // Optional helpers for external monitoring/plotting
  bool     readCkpLevel();
  bool     readGapWindow();
  uint32_t getSlotPeriodUs();

private:
  inline void writePin(bool level);
  inline void writeHigh();
  inline void writeLow();
  static void ARDUINO_ISR_ATTR onTimerStatic();
  void ARDUINO_ISR_ATTR onTimer();

  int         _pin;
  int         _pin_cam1;
  int         _pin_cam2;
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
  // channel_mask bit0 == legacy _invertOutput (crank). Other bits are
  // stored but not acted upon in the legacy adapter (no cam outputs).
  volatile uint8_t  _invertMask;

  // State
  volatile bool     _running;
  volatile bool     _pinHigh;
  volatile uint32_t _slotInPeriod;
  volatile bool     _gapWindow;

  // Edge counter — naturally aligned uint16_t for lock-free readers.
  // Incremented in the ISR on every slot tick; wraps with _slotsPerRev.
  volatile uint16_t _edge_counter;

  // Mirror of the last applied SignalConfig — used by the synthesized
  // runtime byte table for inspection. Allocated lazily in
  // applySignalConfig(); freed in the destructor. Optional / debug-only
  // in M0.1; M1.1 replaces it with a real driving table.
  uint8_t*  _synth_table;
  uint16_t  _synth_slot_count;
  uint32_t  _last_rpm;


  portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
  static TimerCkpGenerator* s_inst;

public:
  ~TimerCkpGenerator() override;
};
