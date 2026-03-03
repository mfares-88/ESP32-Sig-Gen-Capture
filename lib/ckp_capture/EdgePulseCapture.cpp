#include "EdgePulseCapture.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "soc/gpio_reg.h"

EdgePulseCapture* EdgePulseCapture::s_cap = nullptr;

static inline int fastGpioRead(int pin) {
  if (pin < 32) {
    return (REG_READ(GPIO_IN_REG) & (1U << pin)) ? HIGH : LOW;
  }
  return (REG_READ(GPIO_IN1_REG) & (1U << (pin - 32))) ? HIGH : LOW;
}

EdgePulseCapture::EdgePulseCapture() : _pin(-1), _q(nullptr), _lastRise(0), _lastLevel(0), _pending{0, 0, 0} {}

bool EdgePulseCapture::begin(int pin) {
  _pin = pin;
  pinMode(_pin, INPUT);

  _q = xQueueCreate(16, sizeof(CaptureReport));
  if (!_q) return false;

  s_cap = this;
  _lastLevel = fastGpioRead(_pin);
  _lastRise = 0;

  attachInterrupt(digitalPinToInterrupt(_pin), &EdgePulseCapture::onEdgeStatic, CHANGE);
  return true;
}

bool EdgePulseCapture::fetch(CaptureReport& out, uint32_t timeout_ms) {
  if (!_q) return false;
  return xQueueReceive(_q, &out, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void ARDUINO_ISR_ATTR EdgePulseCapture::onEdgeStatic() {
  if (s_cap) s_cap->onEdgeISR();
}

void ARDUINO_ISR_ATTR EdgePulseCapture::onEdgeISR() {
  const int level = fastGpioRead(_pin);
  const uint32_t now = micros();

  if (level == HIGH && _lastLevel == LOW) {
    if (_lastRise != 0) {
      _pending.period_us = now - _lastRise;
      _pending.timestamp_us = now;
      _pending.high_us = 0;
    }
    _lastRise = now;
  }

  if (level == LOW && _lastLevel == HIGH) {
    if (_lastRise != 0) {
      _pending.high_us = now - _lastRise;
      if (_pending.period_us > 0) {
        const CaptureReport report = _pending;
        BaseType_t hpw = pdFALSE;
        if (_q) (void)xQueueSendFromISR(_q, &report, &hpw);
        _pending.period_us = 0;
      }
    }
  }

  _lastLevel = level;
}
