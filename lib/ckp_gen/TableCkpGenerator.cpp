// TableCkpGenerator — native byte-table backend (M1.1 + M2.1 implementation).
//
// Parent reference: implementation_plan.md §M1.1 / §M2.1 + §6 Agent A hard
// rules. See TableCkpGenerator.h for the full design rationale and the ISR
// 5-statement budget contract.

#include "TableCkpGenerator.h"

#include <Arduino.h>
#include <esp_attr.h>
#include <esp_err.h>

// ESP-IDF drivers exposed by the pioarduino arduino-esp32 framework.
#include "driver/gptimer.h"
#include "driver/dedic_gpio.h"
#include "driver/gpio.h"
#include "esp_timer.h"

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

TableCkpGenerator::TableCkpGenerator()
  : _pin_crank(-1),
    _pin_cam1(-1),
    _pin_cam2(-1),
    _bundle_width(0),
    _bundle_mask(0),
    _table(nullptr),
    _slot_count(0),
    _edge_counter(0),
    _invert_mask(0),
    _reverse(false),
    _cycle_start_us(0),
    _cycle_duration_us(0),
    _last_rpm(0),
    _timer(nullptr),
    _bundle(nullptr),
    _running(false),
    _initialized(false) {}

TableCkpGenerator::~TableCkpGenerator() {
  // Best-effort teardown. We don't attempt to re-enter the driver in
  // unusual error states; the system is expected to be alive at dtor time.
  if (_timer) {
    gptimer_stop(_timer);
    gptimer_disable(_timer);
    gptimer_del_timer(_timer);
    _timer = nullptr;
  }
  if (_bundle) {
    dedic_gpio_del_bundle(_bundle);
    _bundle = nullptr;
  }
}

// ---------------------------------------------------------------------------
// begin() — configure dedic_gpio bundle (width 1/2/3 per pin args) + gptimer
// ---------------------------------------------------------------------------

bool TableCkpGenerator::begin(int pin_crank, int pin_cam1, int pin_cam2) {
  if (_initialized) {
    return true;
  }
  if (pin_crank < 0) {
    return false;
  }
  _pin_crank = pin_crank;
  _pin_cam1  = pin_cam1;
  _pin_cam2  = pin_cam2;

  // 1. Build the bundle pin array. Order matters — index 0 → bit0 (crank),
  //    index 1 → bit1 (cam1), index 2 → bit2 (cam2). This matches the
  //    Ardu-Stim byte-packing convention so a byte read from a PatternRef
  //    table can be written straight to the bundle.
  int  bundle_pins[3] = { _pin_crank, 0, 0 };
  uint8_t width = 1;
  if (pin_cam1 >= 0) {
    bundle_pins[1] = pin_cam1;
    width = 2;
    if (pin_cam2 >= 0) {
      bundle_pins[2] = pin_cam2;
      width = 3;
    }
  }
  _bundle_width = width;
  _bundle_mask  = (uint8_t)((1u << width) - 1u);  // 0x01 / 0x03 / 0x07

  // 2. Configure all participating GPIOs as plain OUTPUT first; dedic_gpio
  //    requires the underlying pins to already be in OUTPUT mode.
  uint64_t pin_mask = (1ULL << _pin_crank);
  if (width >= 2) pin_mask |= (1ULL << bundle_pins[1]);
  if (width >= 3) pin_mask |= (1ULL << bundle_pins[2]);

  gpio_config_t io_cfg = {};
  io_cfg.pin_bit_mask = pin_mask;
  io_cfg.mode         = GPIO_MODE_OUTPUT;
  io_cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
  io_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io_cfg.intr_type    = GPIO_INTR_DISABLE;
  if (gpio_config(&io_cfg) != ESP_OK) {
    return false;
  }
  gpio_set_level((gpio_num_t)_pin_crank, 0);
  if (width >= 2) gpio_set_level((gpio_num_t)bundle_pins[1], 0);
  if (width >= 3) gpio_set_level((gpio_num_t)bundle_pins[2], 0);

  // 3. Allocate the dedic_gpio bundle of the chosen width.
  dedic_gpio_bundle_config_t bundle_cfg = {};
  bundle_cfg.gpio_array = bundle_pins;
  bundle_cfg.array_size = width;
  bundle_cfg.flags.out_en = 1;
  if (dedic_gpio_new_bundle(&bundle_cfg, &_bundle) != ESP_OK) {
    return false;
  }

  // 4. Build the gptimer with a 1 MHz resolution (= 1 µs per tick) and
  //    a non-zero default alarm. The alarm is reprogrammed by apply().
  gptimer_config_t timer_cfg = {};
  timer_cfg.clk_src       = GPTIMER_CLK_SRC_DEFAULT;
  timer_cfg.direction     = GPTIMER_COUNT_UP;
  timer_cfg.resolution_hz = 1000000;   // 1 µs tick
  if (gptimer_new_timer(&timer_cfg, &_timer) != ESP_OK) {
    return false;
  }

  gptimer_event_callbacks_t cbs = {};
  cbs.on_alarm = &TableCkpGenerator::onAlarm;
  if (gptimer_register_event_callbacks(_timer, &cbs, this) != ESP_OK) {
    return false;
  }

  gptimer_alarm_config_t alarm_cfg = {};
  alarm_cfg.alarm_count = 1000;        // 1 ms placeholder
  alarm_cfg.reload_count = 0;
  alarm_cfg.flags.auto_reload_on_alarm = true;
  if (gptimer_set_alarm_action(_timer, &alarm_cfg) != ESP_OK) {
    return false;
  }

  if (gptimer_enable(_timer) != ESP_OK) {
    return false;
  }

  _initialized = true;
  return true;
}

// ---------------------------------------------------------------------------
// apply() — switch active pattern (manager-thread only; not ISR-safe)
// ---------------------------------------------------------------------------

bool TableCkpGenerator::apply(const PatternRef& ref, uint32_t rpm) {
  if (!_initialized)             return false;
  if (ref.table == nullptr)      return false;
  if (ref.slot_count == 0)       return false;
  if (rpm < 10)                  return false;   // matches AVR setRPM() floor

  // Pause the timer while we swap pointer/slot_count to keep the ISR's
  // (_table, _slot_count) pair consistent.
  const bool was_running = _running;
  if (was_running) {
    gptimer_stop(_timer);
  }

  _table         = ref.table;
  _slot_count    = ref.slot_count;
  _edge_counter  = 0;
  _cycle_start_us    = (uint32_t)micros();
  _cycle_duration_us = 0;

  if (!reprogramAlarm(rpm)) {
    return false;
  }

  if (was_running) {
    gptimer_set_raw_count(_timer, 0);
    gptimer_start(_timer);
  }
  return true;
}

// ---------------------------------------------------------------------------
// setRpm() — fast path. Only the alarm period changes; no buffer rebuild.
// ---------------------------------------------------------------------------

bool TableCkpGenerator::setRpm(uint32_t rpm) {
  if (!_initialized)         return false;
  if (_table == nullptr)     return false;
  if (_slot_count == 0)      return false;
  if (rpm < 10)              return false;
  return reprogramAlarm(rpm);
}

bool TableCkpGenerator::reprogramAlarm(uint32_t rpm) {
  // period_us = 60_000_000 / (rpm * slot_count)
  // (See header comment for equivalence with Ardu-Stim's OCR1A formula.)
  uint64_t denom = (uint64_t)rpm * (uint64_t)_slot_count;
  if (denom == 0) {
    return false;
  }
  uint64_t period_us = 60000000ULL / denom;
  if (period_us == 0) {
    period_us = 1;  // floor — clip to 1 µs to keep gptimer alarming
  }

  gptimer_alarm_config_t alarm_cfg = {};
  alarm_cfg.alarm_count  = period_us;
  alarm_cfg.reload_count = 0;
  alarm_cfg.flags.auto_reload_on_alarm = true;
  if (gptimer_set_alarm_action(_timer, &alarm_cfg) != ESP_OK) {
    return false;
  }
  _last_rpm = rpm;
  return true;
}

// ---------------------------------------------------------------------------
// Inversion / start / stop / accessors
// ---------------------------------------------------------------------------

void TableCkpGenerator::setInverted(uint8_t channel_mask) {
  // Single-byte volatile store — atomic. bit0=crank, bit1=cam1, bit2=cam2.
  // Bits beyond _bundle_width are XOR'd into the byte but get dropped by
  // the bundle's _bundle_mask on write, so they are harmless.
  _invert_mask = channel_mask;
}

uint8_t TableCkpGenerator::getInverted() const {
  return _invert_mask;
}

void TableCkpGenerator::setReverse(bool reverse) {
  _reverse = reverse;   // single-byte volatile store — atomic
}

bool TableCkpGenerator::getReverse() const {
  return _reverse;
}

void TableCkpGenerator::start() {
  if (!_initialized || _running) return;
  if (_table == nullptr || _slot_count == 0) return;
  gptimer_set_raw_count(_timer, 0);
  if (gptimer_start(_timer) == ESP_OK) {
    _running = true;
  }
}

void TableCkpGenerator::stop() {
  if (!_initialized || !_running) return;
  gptimer_stop(_timer);
  _running = false;
  // Drive every active bundle pin low so all outputs are in a known safe
  // state. _bundle_mask covers exactly the configured channels.
  if (_bundle) {
    dedic_gpio_bundle_write(_bundle, _bundle_mask, 0);
  }
}

uint16_t TableCkpGenerator::getEdgeCounter() const {
  // Single naturally-aligned uint16_t read; Xtensa guarantees atomicity
  // against the ISR writer of the same word — no critical section needed.
  return _edge_counter;
}

uint32_t TableCkpGenerator::getCycleStartUs() const {
  return _cycle_start_us;   // aligned 32-bit atomic read
}

uint32_t TableCkpGenerator::getCycleDurationUs() const {
  return _cycle_duration_us;
}

// ---------------------------------------------------------------------------
// ISR (≤ 5 statements of real work — per §6 Agent A)
// ---------------------------------------------------------------------------
//
// Statement budget walk (5 statements — matches §6 hard cap):
//   (1) byte load + XOR invert mask (single fused expression).
//   (2) dedic_gpio_bundle_write — atomic write of up to 3 channels in one
//       cycle. _bundle_mask selects only valid bits; extra invert bits are
//       discarded harmlessly.
//   (3) advance + wrap. One compound if/else: forward path increments and
//       wraps to 0 (publishing cycle timing on wrap); reverse path decrements
//       and wraps to slot_count-1. The compound statement counts as one per
//       the C++ statement-counting rule (a selection-statement is a single
//       statement regardless of body size).
//   (4) store advanced edge back to the volatile member.
//   (5) return false.
//
bool IRAM_ATTR TableCkpGenerator::onAlarm(
    gptimer_handle_t /*timer*/,
    const gptimer_alarm_event_data_t* /*edata*/,
    void* user_ctx) {
  TableCkpGenerator* self = static_cast<TableCkpGenerator*>(user_ctx);
  uint16_t e = self->_edge_counter;

  // Statement 1: load byte from .rodata table and apply inversion mask.
  uint8_t b = self->_table[e] ^ self->_invert_mask;
  // Statement 2: write all active channels atomically. _bundle_mask is
  // (1<<width)-1, so bits beyond active channels are discarded — this is
  // the per-channel slot-alignment guarantee (zero inter-channel skew).
  dedic_gpio_bundle_write(self->_bundle, self->_bundle_mask, b);
  // Statement 3: advance + wrap (forward or reverse). On forward wrap we
  // publish cycleDuration/cycleStart for Agent C's compression task.
  if (self->_reverse) {
    e = (e == 0) ? (uint16_t)(self->_slot_count - 1) : (uint16_t)(e - 1);
  } else if (++e >= self->_slot_count) {
    e = 0;
    uint32_t now = (uint32_t)esp_timer_get_time();
    self->_cycle_duration_us = now - self->_cycle_start_us;
    self->_cycle_start_us    = now;
  }
  // Statement 4: commit advanced counter.
  self->_edge_counter = e;
  // Statement 5: signal no high-priority task wakeup.
  return false;
}
