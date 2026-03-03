# AGENTS.md - ESP32 Crankshaft Signal Generator

This file contains build commands, code style guidelines, and architectural patterns for agentic coding agents working in this ESP32 signal generator repository.

## Build & Development Commands

### PlatformIO Commands
```bash
# Build for primary development environment (ESP32-S3 with PSRAM)
pio run -e esp32-s3-n4r8

# Upload to device
pio run -e esp32-s3-n4r8 -t upload

# Clean build
pio run -e esp32-s3-n4r8 -t clean

# Serial monitor (921600 baud, LF line endings)
pio device monitor -e esp32-s3-n4r8

# Alternative environment (ESP32-WROOM-32D, no PSRAM)
pio run -e esp32-wroom32d
```

### Testing & Validation
```bash
# No formal test framework - use serial monitor for validation
pio device monitor -e esp32-s3-n4r8

# Test CLI commands via serial:
# rpm <value>         - Set RPM (100-6000)
# pattern <index>     - Select predefined pattern (0-4)
# custom rpm=...      - Build custom pattern
# start/stop          - Control generator
```

## Project Architecture

### Core Components
- **src/main.cpp**: Main orchestration, FreeRTOS tasks, CLI, signal wiring
- **lib/ckp_gen/**: Timer-based CKP generator with ISR implementation
- **lib/ui_lvgl/**: LVGL UI wrapper with callback system
- **Communication**: FreeRTOS queues (`gCtrlQ`) for thread-safe messaging

### Hardware Configuration
- **Primary Target**: ESP32-S3-N4R8 (4MB flash, 8MB OPI PSRAM)
- **Pins**: Defined in main.cpp (`PIN_CKP_OUT`, `PIN_CAPTURE_IN`)
- **Serial**: 921600 baud, LF endings, exception decoder enabled

## Code Style Guidelines

### General Principles
- **Modular Separation**: Hardware/timing in `lib/ckp_gen`, UI in `lib/ui_lvgl`, orchestration in `src/main.cpp`
- **ISR Safety**: Keep ISRs short, use queues, avoid heap allocations
- **Thread Safety**: Use `portENTER_CRITICAL`/`portEXIT_CRITICAL` for shared volatile state
- **Memory**: Prefer stack allocation, minimize dynamic memory in ISRs

### Naming Conventions
```cpp
// Classes: PascalCase
class TimerCkpGenerator;
class EdgePulseCapture;

// Functions: camelCase for most, snake_case for FreeRTOS callbacks
static void pollSerialForDemo();
void managerTask(void* pvParameters);

// Variables: camelCase, prefix indicators
TimerCkpGenerator genTX;
uint32_t _slotPeriod_us;
volatile bool _running;

// Constants: UPPER_CASE with underscores
#define PIN_CKP_OUT        17
#define DEBUG 1

// Enums: PascalCase, enum values UPPER_CASE
enum GapPosition { GAP_AT_END, GAP_AT_START };
```

### Import Organization
```cpp
// 1. Arduino/PlatformIO core
#include <Arduino.h>

// 2. FreeRTOS
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

// 3. ESP32 hardware/drivers
#include "soc/gpio_reg.h"
#include "driver/gpio.h"
#include "esp32-hal-timer.h"

// 4. Project libraries (local includes)
#include "CkpGenerator.h"
#include "ui_lvgl.h"
```

### Error Handling
```cpp
// Use debug macros consistently
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

// Function validation: return bool, use early returns
bool apply(const SignalConfig& cfg) {
  if (cfg.nTeeth == 0 || cfg.rpm == 0 || cfg.pMiss == 0) return false;
  // ... implementation
  return true;
}

// ISR error handling: minimal, use flags
static void ARDUINO_ISR_ATTR onTimerStatic() {
  if (s_inst) s_inst->onTimer();
}
```

### Type Safety & Formatting
```cpp
// Use explicit types from configuration structs
struct SignalConfig {
  uint32_t   rpm;        // 100..6000
  uint16_t   nTeeth;     // total teeth if no gaps
  uint8_t    pMiss;      // gap periods per revolution
  uint8_t    nMiss;      // missing teeth per period
  GapPosition gapPos;    // START or END of period
  bool        gapLvl;    // gap level (false=LOW, true=HIGH)
};

// Volatile for ISR-shared variables
volatile uint32_t _slotPeriod_us;
volatile bool     _running;

// Use standard Arduino types where appropriate
uint32_t timestamp_us = micros();
int level = digitalRead(_pin);
```

## Critical Patterns & Integration Points

### Generator Configuration
```cpp
// Always use SignalConfig for generator changes
SignalConfig cfg{1800, 36, 1, 2, GAP_AT_END, false};
genTX.apply(cfg);  // Computes derived values atomically
```

### UI Integration
```cpp
// UI callback pattern
static void on_ui_rpm(uint32_t rpm) {
  CtrlMsg m;
  m.type = MSG_SET_RPM;
  m.payload.val = (int32_t)rpm;
  xQueueSend(gCtrlQ, &m, 0);
}

// Initialize UI with callbacks
ui_init(on_ui_rpm, on_ui_pattern);

// Service UI frequently
void loop() {
  ui_task_handler();
  vTaskDelay(pdMS_TO_TICKS(10));
}
```

### Message-Based Communication
```cpp
// Control message structure
enum MsgType : uint8_t { MSG_SET_RPM, MSG_SET_PATTERN, MSG_START, MSG_STOP, MSG_SET_CUSTOM };
union MsgPayload {
  int32_t      val;    // For simple commands
  SignalConfig cfg;    // For custom configurations
};
struct CtrlMsg {
  MsgType    type;
  MsgPayload payload;
};

// Queue-based messaging
QueueHandle_t gCtrlQ = xQueueCreate(16, sizeof(CtrlMsg));
xQueueSend(gCtrlQ, &m, 0);
```

## Dependencies & External Libraries

### PlatformIO Dependencies (from platformio.ini)
- **lvgl/lvgl@^9.2.2**: Graphics library
- **moononournation/GFX Library for Arduino@^1.5.6**: Display driver
- **moononournation/Dev Device Pins@^0.0.2**: Pin definitions
- **TAMC_GT911@^1.0.2**: Touch controller driver

### Build Configuration
- **Platform**: pioarduino/platform-espressif32 (stable)
- **Framework**: Arduino
- **Board**: esp32-s3-devkitc-1 (primary)
- **CPU**: 240MHz, Flash: 80MHz DIO
- **PSRAM**: OPI type, qio_opi memory mode
- **Partitions**: huge_app.csv

## Serial Interface & CLI

### Monitor Settings
- **Baud Rate**: 921600
- **Line Endings**: LF
- **Echo**: Enabled
- **Filters**: send_on_enter, time, esp32_exception_decoder

### CLI Commands (case-insensitive)
```bash
rpm <value>         # Set RPM (100-6000)
pattern <index>     # Select pattern (0-4)
custom rpm=...      # Custom pattern builder
start               # Start generation
stop                # Stop generation
status              # Show current config
help                # Show help
```

## Development Workflow

### Making Changes
1. **Generator Logic**: Modify `lib/ckp_gen/CkpGenerator.cpp`, preserve ISR patterns
2. **UI Changes**: Update `lib/ui_lvgl/ui_lvgl.cpp`, maintain callback interface
3. **CLI/Control**: Edit `src/main.cpp` functions, preserve message queue usage
4. **Pin Changes**: Update defines in `src/main.cpp`, call appropriate `begin()` methods

### Validation Steps
1. Build with `pio run -e esp32-s3-n4r8`
2. Upload and monitor with `pio device monitor -e esp32-s3-n4r8`
3. Test CLI commands, verify signal output
4. Check for serial exceptions or crashes

### Common Issues
- **ISR Timing**: Keep ISR operations minimal, use queues for data transfer
- **Memory**: PSRAM available but prefer stack allocation in time-critical code
- **Serial**: Ensure 921600 baud and LF endings for proper CLI operation
- **Threading**: Use FreeRTOS tasks and queues, avoid blocking in main loop

## File Structure Summary
```
src/main.cpp                    # Main application, CLI, FreeRTOS tasks
lib/ckp_gen/CkpGenerator.h      # Generator interface and config
lib/ckp_gen/CkpGenerator.cpp    # Timer-based generator implementation
lib/ui_lvgl/ui_lvgl.h           # UI interface and callbacks
lib/ui_lvgl/ui_lvgl.cpp         # LVGL UI implementation
platformio.ini                  # Build configuration and dependencies
```