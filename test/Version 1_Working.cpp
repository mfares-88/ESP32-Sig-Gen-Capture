/*
  ESP32-S3 (Arduino-ESP32 Core v3)
  CKP Generator (Timer ISR) on GPIO5
  Capture (edge interrupt) on GPIO16
  Serial Plotter channels:
    1) CKP_level
    2) CAP_level
    3) CAP_period_us
    4) CAP_high_us
    5) GAP_WIN   <-- 1 during missing-tooth window, else 0
*/

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "soc/gpio_reg.h"
#include "driver/gpio.h"

// ==========================================================
// Forward declarations (for IntelliSense / C++ compilation)
// ==========================================================
struct SignalConfig;
struct CaptureReport;

class TimerCkpGenerator;
class EdgePulseCapture;

static void tunePlotterSample();          // adapts Serial Plotter sample period
static void plotterPump();                // prints tab-separated values
static void pollSerialForDemo();          // simple CLI while GUI not integrated
void managerTask(void*);                  // FreeRTOS manager task (Core 1)
void setup();                             // Arduino lifecycle
void loop();                              // Arduino lifecycle

// -------------------- Pins --------------------
#define PIN_CKP_OUT        5    // CKP output
#define PIN_CAPTURE_IN     16   // Capture input (GPIO 16)

// -------------------- Debug -------------------
#define DEBUG 1
#if DEBUG
  #define DBG_BEGIN()     Serial.begin(921600)
  #define DBG_PRINTLN(x)  Serial.println(x)
  #define DBG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
  #define DBG_BEGIN()
  #define DBG_PRINTLN(x)
  #define DBG_PRINTF(...)
#endif

// -------------------- Config types --------------------

// NEW: Enum to define the position of the sync gap within a period.
enum GapPosition {
  GAP_AT_END,   // Standard for most automotive patterns (e.g., 60-2)
  GAP_AT_START  // Less common, but exists in some systems
};

struct SignalConfig {
  uint32_t rpm;      // 100..6000
  uint16_t nTeeth;   // e.g. 60, 36
  uint8_t  pMiss;    // e.g. 1 or 2 (Number of gap periods per revolution)
  uint8_t  nMiss;    // e.g. 2 or 1 (Number of missing teeth per gap)

  // NEW: Configuration for gap position and signal level.
  GapPosition gapPos;   // GAP_AT_START or GAP_AT_END
  bool        gapLvl;   // Level of the gap signal (false=LOW, true=HIGH)
};

// -------------------- Interfaces --------------------
struct IGenerator {
  virtual void begin(int pin) = 0;
  virtual void apply(const SignalConfig& cfg) = 0;
  virtual void start() = 0;
  virtual void stop() = 0;
  virtual ~IGenerator() = default;
};

struct CaptureReport {
  uint32_t period_us;
  uint32_t high_us;
  uint32_t timestamp_us;
};

struct ICapture {
  virtual void begin(int pin) = 0;
  virtual bool fetch(CaptureReport& out, uint32_t timeout_ms) = 0;
  virtual ~ICapture() = default;
};


// =====================================================
// TIMER-ISR CKP GENERATOR (slot-based)
// =====================================================
// TIMER-ISR CKP GENERATOR (slot-based)
// =====================================================
class TimerCkpGenerator : public IGenerator {
public:
  TimerCkpGenerator()
  : _pin(-1), _timer(nullptr),
    _slotPeriod_us(500), _slotsPerRev(120), _slotsPerPeriod(120),
    _gapSlots(4), _gapPos(GAP_AT_END), _gapLvl(false),
    _tick(0), _running(false), _pinHigh(false),
    _slotInPeriod(0), _gapWindow(false) {}

  void begin(int pin) override {
    _pin = pin;
    pinMode(_pin, OUTPUT);
    writeLow();

    _timer = timerBegin(1000000); // 1 MHz base (1 tick = 1 µs)
    timerAttachInterrupt(_timer, &TimerCkpGenerator::onTimerStatic);
    s_inst = this;
  }

  void apply(const SignalConfig& cfg) override {
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
    _gapPos         = cfg.gapPos; // Store the new gap position
    _gapLvl         = cfg.gapLvl; // Store the new gap level
    _tick = 0;
    _pinHigh = false;
    _slotInPeriod = 0;
    _gapWindow = false;
    writeLow();
    portEXIT_CRITICAL(&_mux);

    if (_running && _timer) {
      timerAlarm(_timer, _slotPeriod_us, true, 0);
    }

    DBG_PRINTF("[CKP] apply: rpm=%u, teeth=%u-%u, periods=%u, gapPos=%s, gapLvl=%s\n",
               cfg.rpm, cfg.nTeeth, cfg.nMiss, cfg.pMiss,
               _gapPos == GAP_AT_END ? "END" : "START",
               _gapLvl ? "HIGH" : "LOW");
  }

  void start() override {
    if (!_timer) return;
    portENTER_CRITICAL(&_mux);
    _tick = 0; _pinHigh = false; _running = true;
    _slotInPeriod = 0; _gapWindow = false;
    writeLow();
    portEXIT_CRITICAL(&_mux);
    timerAlarm(_timer, _slotPeriod_us, true, 0);
    DBG_PRINTLN("[CKP] generator START");
  }

  void stop() override {
    portENTER_CRITICAL(&_mux);
    _running = false; writeLow();
    portEXIT_CRITICAL(&_mux);
    DBG_PRINTLN("[CKP] generator STOP");
  }

  // --------- Debug getters for plotter (thread-safe) ---------
  bool     readCkpLevel()        { bool v; portENTER_CRITICAL(&_mux); v=_pinHigh;      portEXIT_CRITICAL(&_mux); return v; }
  bool     readGapWindow()       { bool v; portENTER_CRITICAL(&_mux); v=_gapWindow;    portEXIT_CRITICAL(&_mux); return v; }
  
  // FIX: Restore the public getter function for thread-safe access from tunePlotterSample()
  uint32_t getSlotPeriodUs()     { uint32_t v; portENTER_CRITICAL(&_mux); v=_slotPeriod_us; portEXIT_CRITICAL(&_mux); return v; }

private:
  inline void writePin(bool level) {
    REG_WRITE(level ? GPIO_OUT_W1TS_REG : GPIO_OUT_W1TC_REG, (1U << _pin));
    _pinHigh = level;
  }
  inline void writeHigh() { writePin(true); }
  inline void writeLow()  { writePin(false); }

  static void ARDUINO_ISR_ATTR onTimerStatic() {
    if (s_inst) s_inst->onTimer();
  }
  
  void onTimer() {
    portENTER_CRITICAL_ISR(&_mux);
    if (!_running) { portEXIT_CRITICAL_ISR(&_mux); return; }

    // [INC] Advance tick 0.._slotsPerRev-1
    
    if (_tick >= _slotsPerRev-1) _tick = 0;

    // Calculate the current slot within the repeating period (e.g., 0-59 for 60-2)
    uint32_t sip = _tick % _slotsPerPeriod;
    _slotInPeriod = sip;
    
    bool level;
    bool in_gap;

    if (_gapPos == GAP_AT_END) {
        // --- Logic for Gap at the END of the period ---
        in_gap = (sip >= (_slotsPerPeriod - _gapSlots));
        if (in_gap) {
            level = _gapLvl; // Set the configured gap level (HIGH or LOW)
        } else {
            // Generate normal teeth.
            // Starts HIGH at sip=0, alternates. Last tooth slot is LOW.
            level = ((sip & 1u) == 0u); 
        }

    } else { // GAP_AT_START
        // --- Logic for Gap at the START of the period ---
        in_gap = (sip < _gapSlots);
        if (in_gap) {
            level = _gapLvl; // Set the configured gap level (HIGH or LOW)
        } else {
            // Generate normal teeth, but offset the calculation by the gap size.
            // This ensures the first tooth after the gap starts correctly.
            // If gap is HIGH, first tooth slot must be LOW.
            // If gap is LOW, first tooth slot must be HIGH.
            uint32_t tooth_sip = sip - _gapSlots;
            level = (((tooth_sip & 1u) == 0u) != _gapLvl);
        }
    }

    // Drive pin only if the level has changed.
    if (level != _pinHigh) {
        writePin(level);
    }
    _gapWindow = in_gap;
    _tick = _tick + 1;
    portEXIT_CRITICAL_ISR(&_mux);
  }

  int         _pin;
  hw_timer_t* _timer;

  // Configuration is now copied from SignalConfig into the class
  volatile uint32_t _slotPeriod_us;
  volatile uint32_t _slotsPerRev;
  volatile uint32_t _slotsPerPeriod;
  volatile uint32_t _gapSlots;
  volatile GapPosition _gapPos;
  volatile bool     _gapLvl;

  // State variables
  volatile uint32_t _tick;
  volatile bool     _running;
  volatile bool     _pinHigh;

  // Debug state for plotter
  volatile uint32_t _slotInPeriod;
  volatile bool     _gapWindow;

  portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
  static TimerCkpGenerator* s_inst;
};
TimerCkpGenerator* TimerCkpGenerator::s_inst = nullptr;


// =====================================================
// Edge-interrupt capture (Unchanged from original)
// =====================================================
class EdgePulseCapture : public ICapture {
public:
  EdgePulseCapture() : _pin(-1), _q(nullptr), _lastRise(0), _lastLevel(0) {}

  void begin(int pin) override {
    _pin = pin;
    pinMode(_pin, INPUT);
    _q = xQueueCreate(16, sizeof(CaptureReport));
    s_cap = this;
    _lastLevel = digitalRead(_pin);
    _lastRise  = 0;
    attachInterrupt(digitalPinToInterrupt(_pin), &EdgePulseCapture::onEdgeStatic, CHANGE);
  }

  bool fetch(CaptureReport& out, uint32_t timeout_ms) override {
    if (!_q) return false;
    return xQueueReceive(_q, &out, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
  }

private:
  static void onEdgeStatic() { if (s_cap) s_cap->onEdgeISR(); }

  void onEdgeISR() {
    int level = digitalRead(_pin);
    uint32_t now = micros();

    if (level == HIGH && _lastLevel == LOW) {
      if (_lastRise != 0) {
        _pending.period_us    = now - _lastRise;
        _pending.timestamp_us = now;
        _pending.high_us      = 0;
      }
      _lastRise = now;
    }
    if (level == LOW && _lastLevel == HIGH) {
      if (_lastRise != 0) {
        _pending.high_us = now - _lastRise;
        if (_pending.period_us > 0) {
          CaptureReport r = _pending;
          BaseType_t hpw = pdFALSE;
          if (_q) xQueueSendFromISR(_q, &r, &hpw);
          _pending.period_us = 0;
        }
      }
    }
    _lastLevel = level;
  }

  int _pin;
  QueueHandle_t _q;
  volatile uint32_t _lastRise;
  volatile int _lastLevel;
  CaptureReport _pending{0,0,0};
  static EdgePulseCapture* s_cap;
};
EdgePulseCapture* EdgePulseCapture::s_cap = nullptr;

// =====================================================
// Control / Manager (Core 1)
// =====================================================
enum MsgType : uint8_t { MSG_SET_RPM, MSG_SET_PATTERN, MSG_START, MSG_STOP };
struct CtrlMsg { MsgType type; int32_t val; };
QueueHandle_t gCtrlQ = nullptr;

// NEW: Updated patterns to include the new gap configuration.
static inline SignalConfig patternFromIndex(uint8_t idx, uint32_t rpmCurrent) {
  // Default: A standard 60-2 pattern with a LOW gap at the END.
  SignalConfig c{rpmCurrent, 60, 1, 2, GAP_AT_END, false};
  switch (idx) {
    case 0: c = {rpmCurrent, 60, 1, 2, GAP_AT_END, false}; break; // 60-2 (LOW gap at END)
    case 1: c = {rpmCurrent, 36, 1, 1, GAP_AT_END, false}; break; // 36-1 (LOW gap at END)
    case 2: c = {rpmCurrent, 36, 1, 2, GAP_AT_END, false}; break; // 36-2 (LOW gap at END)
    case 3: c = {rpmCurrent, 36, 2, 1, GAP_AT_END, false}; break; // 36-1-1 (LOW gap at END)
    // Hypothetical pattern to demonstrate a HIGH gap at the START.
    case 4: c = {rpmCurrent, 12, 1, 1, GAP_AT_START, true}; break; // 12-1 (HIGH sync pulse at START)
  }
  return c;
}

TimerCkpGenerator genTX;
EdgePulseCapture  capRX;
// Initial pattern is now set here.
SignalConfig gCfg{1000, 60, 1, 2, GAP_AT_END, false};

// ------------- Plotter and Manager Task (Unchanged) -------------
volatile uint32_t gPlotterSampleUs = 1000;
static void tunePlotterSample() {
  uint32_t slot = genTX.getSlotPeriodUs();
  uint32_t desired = slot / 2;
  if (desired < 300) desired = 300;
  if (desired > 2000) desired = 2000;
  gPlotterSampleUs = desired;
}

void managerTask(void*) {
  CtrlMsg m;
  for (;;) {
    if (xQueueReceive(gCtrlQ, &m, portMAX_DELAY) == pdTRUE) {
      switch (m.type) {
        case MSG_SET_RPM:
          gCfg.rpm = constrain((uint32_t)m.val, 100u, 6000u);
          genTX.apply(gCfg);
          tunePlotterSample();
          break;
        case MSG_SET_PATTERN:
          gCfg = patternFromIndex((uint8_t)m.val, gCfg.rpm);
          genTX.apply(gCfg);
          tunePlotterSample();
          break;
        case MSG_START: genTX.start(); break;
        case MSG_STOP:  genTX.stop();  break;
      }
    }
  }
}

// -------------------- Serial Plotter and Demo CLI (Unchanged) --------------------
volatile uint32_t gCapPeriod = 0;
volatile uint32_t gCapHigh   = 0;

static void plotterPump() {
  static uint32_t last = 0;
  uint32_t now = micros();
  uint32_t interval = gPlotterSampleUs;
  if ((uint32_t)(now - last) >= interval) {
    int ckp = genTX.readCkpLevel() ? 1 : 0;
    int cap = digitalRead(PIN_CAPTURE_IN);
    int gap = genTX.readGapWindow() ? 1 : 0;

    Serial.printf("%d\t%d\t%u\t%u\t%d\n", ckp, cap, (unsigned)gCapPeriod, (unsigned)gCapHigh, gap);
    last = now;
  }
}

static void pollSerialForDemo() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim(); cmd.toLowerCase();
  if (cmd.startsWith("rpm")) {
    uint32_t r = cmd.substring(3).toInt();
    CtrlMsg m{MSG_SET_RPM, (int32_t)r}; xQueueSend(gCtrlQ, &m, 0);
    DBG_PRINTF("[CMD] set RPM -> %u\n", r);
  } else if (cmd.startsWith("pattern")) {
    uint32_t idx = cmd.substring(7).toInt();
    CtrlMsg m{MSG_SET_PATTERN, (int32_t)idx}; xQueueSend(gCtrlQ, &m, 0);
    DBG_PRINTF("[CMD] set pattern index -> %u (0:60-2, 1:36-1, 4:12-1 HI-start)\n", idx);
  } else if (cmd == "start") {
    CtrlMsg m{MSG_START, 0}; xQueueSend(gCtrlQ, &m, 0); DBG_PRINTLN("[CMD] START");
  } else if (cmd == "stop") {
    CtrlMsg m{MSG_STOP, 0};  xQueueSend(gCtrlQ, &m, 0); DBG_PRINTLN("[CMD] STOP");
  } else {
    DBG_PRINTLN("[CMD] Use: 'rpm 1500', 'pattern 2', 'start', 'stop'");
  }
}

// -------------------- Arduino setup/loop --------------------
void setup() {
  DBG_BEGIN();
  DBG_PRINTLN("\nCKP (Core v3) with Configurable Gap Position/Level...");

  genTX.begin(PIN_CKP_OUT);
  capRX.begin(PIN_CAPTURE_IN);

  gCtrlQ = xQueueCreate(16, sizeof(CtrlMsg));
  xTaskCreatePinnedToCore(managerTask, "managerTask", 4096, nullptr, 3, nullptr, 1);

  genTX.apply(gCfg);
  tunePlotterSample();
  genTX.start();

  DBG_PRINTLN("Commands: 'rpm 1500', 'pattern X', 'start', 'stop'");
  DBG_PRINTLN("Patterns: 0:60-2, 1:36-1, 2:36-2, 3:36-1-1, 4:12-1(Hi-Sync@Start)");
}

void loop() {
  pollSerialForDemo();

  CaptureReport r;
  if (capRX.fetch(r, 0)) {
    gCapPeriod = r.period_us;
    gCapHigh   = r.high_us;
  }

  plotterPump();

  vTaskDelay(pdMS_TO_TICKS(1));
}