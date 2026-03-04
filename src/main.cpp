// ESP32 Crankshaft Signal Generator (UI-only control)
// - LVGL UI drives RPM/pattern/start/stop
// - FreeRTOS queue carries UI requests to the manager task
// - Timer ISR in lib/ckp_gen generates the CKP waveform

#include <Arduino.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"

#include "CkpGenerator.h"
#include "EdgePulseCapture.h"
#include "ui_lvgl.h"

// -------------------- Pins --------------------
#define PIN_CKP_OUT        17
#define PIN_CAPTURE_IN     18

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

// =====================================================
// Control / Manager (Core 1)
// =====================================================

enum MsgType : uint8_t { MSG_SET_RPM, MSG_SET_PATTERN, MSG_START, MSG_STOP, MSG_SET_CUSTOM, MSG_SET_INVERT };

union MsgPayload {
  int32_t      val;
  SignalConfig cfg;
};

struct CtrlMsg {
  MsgType    type;
  MsgPayload payload;
};

static QueueHandle_t gCtrlQ = nullptr;

static uint32_t gUiMsgDropCount = 0;
static portMUX_TYPE gUiMsgDropMux = portMUX_INITIALIZER_UNLOCKED;

static inline void bumpUiMsgDropCount() {
  portENTER_CRITICAL(&gUiMsgDropMux);
  ++gUiMsgDropCount;
  portEXIT_CRITICAL(&gUiMsgDropMux);
}

static TimerCkpGenerator genTX;
static EdgePulseCapture  capRX;

static SignalConfig gCfg{1000, 60, 1, 2, GAP_AT_END, false};
static uint8_t gPatternIdx = 0;
static volatile bool gRunning = true;
static volatile bool gInverted = false;

static inline SignalConfig patternFromIndex(uint8_t idx, uint32_t rpmCurrent) {
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

static bool sendCtrlMsg(const CtrlMsg& msg) {
  if (!gCtrlQ) {
    bumpUiMsgDropCount();
    return false;
  }
  if (xQueueSend(gCtrlQ, &msg, 0) != pdTRUE) {
    bumpUiMsgDropCount();
    return false;
  }
  return true;
}

static void on_ui_rpm(uint32_t rpm) {
  CtrlMsg m{};
  m.type = MSG_SET_RPM;
  m.payload.val = (int32_t)rpm;
  if (!sendCtrlMsg(m)) {
    ui_show_error("Control queue full");
    ui_update_rpm(gCfg.rpm);
  }
}

static void on_ui_pattern(uint8_t idx) {
  CtrlMsg m{};
  m.type = MSG_SET_PATTERN;
  m.payload.val = (int32_t)idx;
  if (!sendCtrlMsg(m)) {
    ui_show_error("Control queue full");
    ui_update_pattern(gPatternIdx);
  }
}

static void on_ui_run(bool running) {
  CtrlMsg m{};
  m.type = running ? MSG_START : MSG_STOP;
  if (!sendCtrlMsg(m)) {
    ui_show_error("Control queue full");
    ui_update_running(gRunning);
  }
}

static void on_ui_custom(const SignalConfig& cfg) {
  CtrlMsg m{};
  m.type = MSG_SET_CUSTOM;
  m.payload.cfg = cfg;
  if (!sendCtrlMsg(m)) {
    ui_show_error("Control queue full");
    ui_update_rpm(gCfg.rpm);
    ui_update_pattern(gPatternIdx);
    ui_update_running(gRunning);
  }
}

static void on_ui_invert(bool inverted) {
  CtrlMsg m{};
  m.type = MSG_SET_INVERT;
  m.payload.val = inverted ? 1 : 0;
  if (!sendCtrlMsg(m)) {
    ui_show_error("Control queue full");
    ui_update_inverted(gInverted);
  }
}

void managerTask(void*) {
  CtrlMsg m{};
  SignalConfig lastGood = gCfg;
  uint8_t lastGoodPattern = gPatternIdx;

  for (;;) {
    if (xQueueReceive(gCtrlQ, &m, portMAX_DELAY) != pdTRUE) continue;

    switch (m.type) {
      case MSG_SET_RPM: {
        const uint32_t requested = (uint32_t)m.payload.val;
        const uint32_t clamped = constrain(requested, 100u, 6000u);

        SignalConfig next = gCfg;
        next.rpm = clamped;

        if (genTX.apply(next)) {
          gCfg = next;
          lastGood = gCfg;
          ui_show_error("");
          if (clamped != requested) ui_update_rpm(clamped);
        } else {
          gCfg = lastGood;
          ui_update_rpm(gCfg.rpm);
          ui_show_error("Invalid RPM/config");
        }
        break;
      }

      case MSG_SET_PATTERN: {
        const uint32_t requested = (uint32_t)m.payload.val;
        const uint8_t idx = (requested > 4u) ? 4u : (uint8_t)requested;

        const SignalConfig next = patternFromIndex(idx, gCfg.rpm);
        if (genTX.apply(next)) {
          gCfg = next;
          gPatternIdx = idx;
          lastGood = gCfg;
          lastGoodPattern = gPatternIdx;
          ui_show_error("");
          if (idx != requested) ui_update_pattern(idx);
        } else {
          gCfg = lastGood;
          gPatternIdx = lastGoodPattern;
          ui_update_rpm(gCfg.rpm);
          ui_update_pattern(gPatternIdx);
          ui_show_error("Invalid pattern/config");
        }
        break;
      }

      case MSG_SET_CUSTOM: {
        SignalConfig next = m.payload.cfg;
        next.rpm = constrain(next.rpm, 100u, 6000u);

        if (genTX.apply(next)) {
          gCfg = next;
          lastGood = gCfg;
          ui_show_error("");
          ui_update_rpm(gCfg.rpm);
        } else {
          gCfg = lastGood;
          ui_update_rpm(gCfg.rpm);
          ui_update_pattern(gPatternIdx);
          ui_show_error("Invalid custom config");
        }
        break;
      }

      case MSG_START:
        genTX.start();
        gRunning = true;
        ui_update_running(true);
        break;

      case MSG_STOP:
        genTX.stop();
        gRunning = false;
        ui_update_running(false);
        break;

      case MSG_SET_INVERT: {
        const bool requested = (m.payload.val != 0);
        genTX.setInverted(requested);
        gInverted = requested;
        ui_update_inverted(gInverted);
        ui_show_error("");
        break;
      }
    }
  }
}

// =====================================================
// Arduino setup/loop
// =====================================================

void setup() {
  DBG_BEGIN();
  delay(250);

  const bool genOk = genTX.begin(PIN_CKP_OUT);
  if (!genOk) {
    ui_show_error("Timer init failed");
  }

  const bool capOk = capRX.begin(PIN_CAPTURE_IN);
  if (!capOk) {
    DBG_PRINTLN("[CAP] queue/interrupt init failed");
  }

  const bool uiOk = ui_init(on_ui_rpm, on_ui_pattern, on_ui_run, on_ui_custom, on_ui_invert);
  if (!uiOk) {
    DBG_PRINTLN("[UI] init failed; running defaults only");
  }

  gCtrlQ = xQueueCreate(16, sizeof(CtrlMsg));
  if (!gCtrlQ) {
    ui_show_error("Queue alloc failed");
  } else {
    const BaseType_t ok = xTaskCreatePinnedToCore(managerTask, "managerTask", 4096, nullptr, 3, nullptr, 1);
    if (ok != pdPASS) {
      ui_show_error("Task create failed");
    }
  }

  if (!genTX.apply(gCfg)) {
    ui_show_error("Default config invalid");
  }

  genTX.start();
  gRunning = true;

  ui_update_rpm(gCfg.rpm);
  ui_update_pattern(gPatternIdx);
  ui_update_running(true);
  ui_update_inverted(gInverted);
}

void loop() {
  ui_task_handler();

  CaptureReport r{};
  (void)capRX.fetch(r, 0);

  vTaskDelay(pdMS_TO_TICKS(10));
}
