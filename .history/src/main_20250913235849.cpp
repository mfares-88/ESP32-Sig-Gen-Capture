
/////**************ESP32-CKP Generator and Capture************/
//The code below is working to generate a CKP signal based on input from the user. The operating mechanism is as follows:

// 1. Default build creates a 60-2 CKP signal with RPM 1000, with the gap at the end of the signal.
// 2. To trigger changes from the serial monitor, the following commands are used:
                //      rpm <value>         - Set engine speed (e.g., 'rpm 1500')
                //      pattern <index>     - Select a pre-defined pattern (0-4)
                            //          case 0: c = {rpmCurrent, 60, 1, 2, GAP_AT_END, false}; break; // 60-2
                            //          case 1: c = {rpmCurrent, 36, 1, 1, GAP_AT_END, false}; break; // 36-1
                            //          case 2: c = {rpmCurrent, 36, 1, 2, GAP_AT_END, false}; break; // 36-2
                            //          case 3: c = {rpmCurrent, 36, 2, 1, GAP_AT_END, false}; break; // 36-1-1
                            //          case 4: c = {rpmCurrent, 12, 1, 1, GAP_AT_START, true}; break; // 12-1 (HIGH sync)
                //      custom              - Get help on the custom pattern builder
                //      start               - Start or resume signal generation
                //      stop                - Stop signal generation
// 3. For custom triggers, the following commands are used:
        //Syntax: custom rpm=<val> teeth=<val> pmiss=<val> nmiss=<val> pos=<s|e> lvl=<h|l>
                //Example: custom rpm=1800 teeth=36 pmiss=2 nmiss=1 pos=e lvl=l
                //rpm:   Revolutions Per Minute (100-6000)
                //teeth: Total teeth if there were no gaps (e.g., 60, 36)
                //pmiss: Number of gap periods per revolution (e.g., 1 for 60-2, 2 for 36-1-1)
                //nmiss: Number of missing teeth per period (e.g., 2 for 60-2, 1 for 36-1)
                //pos:   Gap Position ('s' for START, 'e' for END)
                //lvl:   Gap Level ('h' for HIGH, 'l' for LOW)
        //Note: If you just want to modify a single parameter in custom mode, you can type custom <parameter>=x and just press enter.
        //      You don't have to rewrite all the parameters with the custom command.  
                    
        

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
#include "CkpGenerator.h"

// ==========================================================
// Forward declarations (for IntelliSense / C++ compilation)
// ==========================================================
struct CaptureReport;
class EdgePulseCapture;

static void tunePlotterSample();          // adapts Serial Plotter sample period
static void plotterPump();                // prints tab-separated values
static void printHelp();                  // prints high-level help
static void printPatterns();              // prints pattern list
static void printStatus();                // prints current config/state
static void printPrompt();                // prints CLI prompt
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
#if 0 // Moved to library (CkpGenerator.h)

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
#endif // moved to library

// -------------------- Interfaces --------------------
#if 0 // Moved to library (IGenerator in CkpGenerator.h)
struct IGenerator {
  virtual void begin(int pin) = 0;
  virtual void apply(const SignalConfig& cfg) = 0;
  virtual void start() = 0;
  virtual void stop() = 0;
  virtual ~IGenerator() = default;
};
#endif

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
#if 0 // Moved to library (TimerCkpGenerator in CkpGenerator.*)
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
    
    if (_tick == _slotsPerRev-1) _tick = 0;

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
            
            if(!_gapLvl){ // If the requested gap needs a LOW level, the signal right after the gap starts HIGH at sip=0, alternates. Last tooth slot is LOW.
              level = ((sip & 1u) == 0u);
            } else { // If the requested gap needs a HIGH level, the signal right after the gap starts LOW at sip=0, alternates. Last tooth slot is HIGH.
              level = ((sip & 1u) == 1u);
            } 
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
  volatile bool _gapLvl;

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
#endif // moved to library


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

// NEW: The message type enum now includes a type for a fully custom signal.
enum MsgType : uint8_t { MSG_SET_RPM, MSG_SET_PATTERN, MSG_START, MSG_STOP, MSG_SET_CUSTOM };

// NEW: The message payload is now a union. A union is a special data structure
// that can hold different data types at the same memory location. This allows us
// to send either a simple integer (for RPM/pattern index) or a full SignalConfig
// struct efficiently in the same message.
union MsgPayload {
  int32_t      val; // Used for MSG_SET_RPM, MSG_SET_PATTERN
  SignalConfig cfg; // Used for MSG_SET_CUSTOM
};

// The control message now contains the payload union.
struct CtrlMsg {
  MsgType    type;
  MsgPayload payload;
};

QueueHandle_t gCtrlQ = nullptr;

// Pre-defined patterns remain the same.
static inline SignalConfig patternFromIndex(uint8_t idx, uint32_t rpmCurrent) {
  // Default: A standard 60-2 pattern with a LOW gap at the END.
  SignalConfig c{rpmCurrent, 60, 1, 2, GAP_AT_END, false};
  switch (idx) {
    case 0: c = {rpmCurrent, 60, 1, 2, GAP_AT_END, false}; break; // 60-2
    case 1: c = {rpmCurrent, 36, 1, 1, GAP_AT_END, false}; break; // 36-1
    case 2: c = {rpmCurrent, 36, 1, 2, GAP_AT_END, false}; break; // 36-2
    case 3: c = {rpmCurrent, 36, 2, 1, GAP_AT_END, false}; break; // 36-1-1
    case 4: c = {rpmCurrent, 12, 1, 1, GAP_AT_START, true}; break; // 12-1 (HIGH sync)
  }
  return c;
}

TimerCkpGenerator genTX;
EdgePulseCapture  capRX;
SignalConfig gCfg{1000, 60, 1, 2, GAP_AT_END, false}; // Global config still holds current state
static bool gUiRunning = true; // UI's view of running state (for status only)

// The plotter sampling function is no longer needed but kept for potential future use.
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
          gCfg.rpm = constrain((uint32_t)m.payload.val, 100u, 6000u);
          genTX.apply(gCfg);
          break;
        case MSG_SET_PATTERN:
          gCfg = patternFromIndex((uint8_t)m.payload.val, gCfg.rpm);
          genTX.apply(gCfg);
          break;

        // NEW: Handle the fully custom signal configuration.
        case MSG_SET_CUSTOM:
          gCfg = m.payload.cfg; // Update global config with the custom one
          genTX.apply(gCfg);
          break;

        case MSG_START: genTX.start(); break;
        case MSG_STOP:  genTX.stop();  break;
      }
    }
  }
}

// -------------------- Serial Plotter and Demo CLI --------------------

// The plotter pump is no longer used for output but its structure is kept.
static void plotterPump() {
  // This function is now disabled to stop continuous data printing.
}

// -------------- CLI helpers (UI only) --------------
static void printHelp() {
  DBG_PRINTLN("\nCommands (send via Serial Monitor at 921600 baud):");
  DBG_PRINTLN("  rpm <value>         - Set engine speed (e.g., 'rpm 1500')");
  DBG_PRINTLN("    Aliases: '<value>' alone, 'rpm=<v>', 'set rpm <v>'");
  DBG_PRINTLN("  pattern <index>     - Select a pre-defined pattern (0-4)");
  DBG_PRINTLN("    Aliases: 'p <i>', 'pat <i>', 'pattern=<i>', 'set pattern <i>'");
  DBG_PRINTLN("  custom [k=v ...]    - Build a custom pattern; omit args to see details");
  DBG_PRINTLN("  start               - Start or resume signal generation (aliases: go, run)");
  DBG_PRINTLN("  stop                - Stop signal generation (aliases: pause, halt)");
  DBG_PRINTLN("  status              - Show current configuration summary");
  DBG_PRINTLN("  patterns            - List pattern indices and descriptions");
  DBG_PRINTLN("  help                - Show this help (aliases: h, ?)");
}

static void printPatterns() {
  DBG_PRINTLN("\nPatterns:");
  DBG_PRINTLN("  0: 60-2      (gap at END, LOW gap)");
  DBG_PRINTLN("  1: 36-1      (gap at END, LOW gap)");
  DBG_PRINTLN("  2: 36-2      (gap at END, LOW gap)");
  DBG_PRINTLN("  3: 36-1-1    (2 periods/rev, LOW gap)");
  DBG_PRINTLN("  4: 12-1      (gap at START, HIGH gap)");
}

static void printStatus() {
  DBG_PRINTLN("\nStatus:");
  DBG_PRINTF("  Running: %s\n", gUiRunning ? "YES" : "NO");
  DBG_PRINTF("  RPM: %u\n", gCfg.rpm);
  DBG_PRINTF("  Teeth: %u\n", gCfg.nTeeth);
  DBG_PRINTF("  Periods/Rev: %u\n", gCfg.pMiss);
  DBG_PRINTF("  Missing/Period: %u\n", gCfg.nMiss);
  DBG_PRINTF("  Gap Pos: %s\n", (gCfg.gapPos == GAP_AT_START ? "START" : "END"));
  DBG_PRINTF("  Gap Level: %s\n", (gCfg.gapLvl ? "HIGH" : "LOW"));
}

static void printPrompt() {
  Serial.print("> ");
}

// NEW: This function is completely rewritten to handle advanced commands.
static void pollSerialForDemo() {
  if (!Serial.available()) return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  cmd.toLowerCase();

  // If the user just presses Enter, show the help message again.
  if (cmd.length() == 0) {
      printHelp();
      printPrompt();
      return;
  }
  
  // Quick help/status/patterns commands
  if (cmd == "help" || cmd == "h" || cmd == "?") {
    printHelp();
    printPatterns();
    printStatus();
    printPrompt();
    return;
  }
  if (cmd == "status" || cmd == "s") {
    printStatus();
    printPrompt();
    return;
  }
  if (cmd == "patterns" || cmd == "list") {
    printPatterns();
    printPrompt();
    return;
  }

  // Allow bare number to set RPM (e.g., "1500")
  bool isNumber = true;
  for (size_t i = 0; i < cmd.length(); ++i) {
    char c = cmd[i];
    if (c < '0' || c > '9') { isNumber = false; break; }
  }
  if (isNumber) {
    uint32_t r = cmd.toInt();
    CtrlMsg m; m.type = MSG_SET_RPM; m.payload.val = (int32_t)r;
    xQueueSend(gCtrlQ, &m, 0);
    DBG_PRINTF("[CMD] Set RPM -> %u\n", r);
    printPrompt();
    return;
  }

  // Normalize some aliases to existing commands
  if (cmd.startsWith("r ")) cmd = String("rpm") + cmd.substring(1);
  if (cmd.startsWith("rpm=")) cmd = String("rpm ") + cmd.substring(4);
  if (cmd.startsWith("set rpm ")) cmd = String("rpm ") + cmd.substring(8);

  if (cmd.startsWith("p ")) cmd = String("pattern") + cmd.substring(1);
  if (cmd.startsWith("pat ")) cmd = String("pattern") + cmd.substring(3);
  if (cmd.startsWith("pattern=")) cmd = String("pattern ") + cmd.substring(8);
  if (cmd.startsWith("set pattern ")) cmd = String("pattern ") + cmd.substring(12);

  if (cmd == "go" || cmd == "run" || cmd == "resume") cmd = "start";
  if (cmd == "pause" || cmd == "halt") cmd = "stop";

  if (cmd.startsWith("custom")) {
    // If the command is just "custom", print the detailed help for it.
    if (cmd.length() < 7) {
        DBG_PRINTLN("\n--- Custom Pattern Builder ---");
        DBG_PRINTLN("Syntax: custom rpm=<val> teeth=<val> pmiss=<val> nmiss=<val> pos=<s|e> lvl=<h|l>");
        DBG_PRINTLN("Example: custom rpm=1800 teeth=36 pmiss=2 nmiss=1 pos=e lvl=l");
        DBG_PRINTLN("  rpm:   Revolutions Per Minute (100-6000)");
        DBG_PRINTLN("  teeth: Total teeth if there were no gaps (e.g., 60, 36)");
        DBG_PRINTLN("  pmiss: Number of gap periods per revolution (e.g., 1 for 60-2, 2 for 36-1-1)");
        DBG_PRINTLN("  nmiss: Number of missing teeth per period (e.g., 2 for 60-2, 1 for 36-1)");
        DBG_PRINTLN("  pos:   Gap Position ('s' for START, 'e' for END)");
        DBG_PRINTLN("  lvl:   Gap Level ('h' for HIGH, 'l' for LOW)");
        printPrompt();
        return;
    }
    
    // If command has parameters, parse them.
    SignalConfig customCfg = gCfg; // Start with current config as a template
    cmd.remove(0, 6); // Remove the "custom" part
    
    // This loop splits the command string by spaces, then parses each "key=value" pair.
    int currentIndex = 0;
    while(currentIndex < cmd.length()) {
        int nextSpace = cmd.indexOf(' ', currentIndex);
        if (nextSpace == -1) nextSpace = cmd.length();
        
        String pair = cmd.substring(currentIndex, nextSpace);
        currentIndex = nextSpace + 1;

        int eqIndex = pair.indexOf('=');
        if (eqIndex != -1) {
            String key = pair.substring(0, eqIndex);
            String value = pair.substring(eqIndex + 1);

            if (key == "rpm") customCfg.rpm = value.toInt();
            else if (key == "teeth") customCfg.nTeeth = value.toInt();
            else if (key == "pmiss") customCfg.pMiss = value.toInt();
            else if (key == "nmiss") customCfg.nMiss = value.toInt();
            else if (key == "pos" && value.startsWith("s")) customCfg.gapPos = GAP_AT_START;
            else if (key == "pos" && value.startsWith("e")) customCfg.gapPos = GAP_AT_END;
            else if (key == "lvl" && value.startsWith("h")) customCfg.gapLvl = true;
            else if (key == "lvl" && value.startsWith("l")) customCfg.gapLvl = false;
        }
    }

    // Send the complete custom configuration in a single message.
    CtrlMsg m;
    m.type = MSG_SET_CUSTOM;
    m.payload.cfg = customCfg;
    xQueueSend(gCtrlQ, &m, 0);

    // Provide feedback to the user confirming the applied settings.
    DBG_PRINTLN("\n[CMD] Applying Custom Configuration:");
    DBG_PRINTF(" -> RPM: %u, Teeth: %u, Periods: %u, Missing/Period: %u, Pos: %s, Lvl: %s\n",
        customCfg.rpm, customCfg.nTeeth, customCfg.pMiss, customCfg.nMiss,
        customCfg.gapPos == GAP_AT_START ? "START" : "END",
        customCfg.gapLvl ? "HIGH" : "LOW");
    printPrompt();

  } else if (cmd.startsWith("rpm")) {
    uint32_t r = cmd.substring(3).toInt();
    CtrlMsg m;
    m.type = MSG_SET_RPM;
    m.payload.val = (int32_t)r;
    xQueueSend(gCtrlQ, &m, 0);
    DBG_PRINTF("[CMD] Set RPM -> %u\n", r);
    printPrompt();
    
  } else if (cmd.startsWith("pattern")) {
    uint32_t idx = cmd.substring(7).toInt();
    CtrlMsg m;
    m.type = MSG_SET_PATTERN;
    m.payload.val = (int32_t)idx;
    xQueueSend(gCtrlQ, &m, 0);
    DBG_PRINTF("[CMD] Set pattern index -> %u\n", idx);
    printPrompt();
    
  } else if (cmd == "start") {
    CtrlMsg m;
    m.type = MSG_START;
    xQueueSend(gCtrlQ, &m, 0);
    DBG_PRINTLN("[CMD] ==> Generator START");
    gUiRunning = true;
    printPrompt();
    
  } else if (cmd == "stop") {
    CtrlMsg m;
    m.type = MSG_STOP;
    xQueueSend(gCtrlQ, &m, 0);
    DBG_PRINTLN("[CMD] ==> Generator STOP");
    gUiRunning = false;
    printPrompt();
    
  } else {
    DBG_PRINTLN("[CMD] Unknown command. Type 'help' for usage.");
    printPrompt();
  }
}

// -------------------- Arduino setup/loop --------------------
void setup() {
  DBG_BEGIN();
  delay(1000); // Wait a moment for serial monitor to connect.
  DBG_PRINTLN("\n==================================================");
  DBG_PRINTLN("===== ESP32 Crankshaft Signal Generator =====");
  DBG_PRINTLN("==================================================");
  printHelp();
  printPatterns();
  printStatus();
  DBG_PRINTLN("--------------------------------------------------");
  printPrompt();
  
  genTX.begin(PIN_CKP_OUT);
  capRX.begin(PIN_CAPTURE_IN);

  // The size of the queue must match the size of the new CtrlMsg struct.
  gCtrlQ = xQueueCreate(16, sizeof(CtrlMsg));
  xTaskCreatePinnedToCore(managerTask, "managerTask", 4096, nullptr, 3, nullptr, 1);

  // Apply the default configuration and start the generator.
  genTX.apply(gCfg);
  genTX.start();
}

void loop() {
  // Check for new user commands from the serial monitor.
  pollSerialForDemo();

  // The capture logic can remain, as it doesn't print anything.
  // It simply fetches data into variables if a signal is present on the input pin.
  CaptureReport r;
  if (capRX.fetch(r, 0)) {
    // Data is fetched, but not displayed.dd
  }

  // The plotter function is no longer called.
  // plotterPump();

  // A small delay is crucial in the loop to allow other FreeRTOS tasks to run.
  vTaskDelay(pdMS_TO_TICKS(10));
}
