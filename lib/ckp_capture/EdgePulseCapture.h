#pragma once

#include <Arduino.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

struct CaptureReport {
  uint32_t period_us;
  uint32_t high_us;
  uint32_t timestamp_us;
};

class EdgePulseCapture {
public:
  EdgePulseCapture();

  bool begin(int pin);
  bool fetch(CaptureReport& out, uint32_t timeout_ms);

private:
  static void ARDUINO_ISR_ATTR onEdgeStatic();
  void ARDUINO_ISR_ATTR onEdgeISR();

  int _pin;
  QueueHandle_t _q;
  volatile uint32_t _lastRise;
  volatile int _lastLevel;
  CaptureReport _pending;

  static EdgePulseCapture* s_cap;
};
