// Minimal LVGL UI wrapper for ESP32-S3
#pragma once

#include <lvgl.h>
#include <stdint.h>

#include "CkpGenerator.h"

typedef void (*ui_on_rpm_cb)(uint32_t rpm);
typedef void (*ui_on_pattern_cb)(uint8_t pattern_index);
typedef void (*ui_on_run_cb)(bool running);
typedef void (*ui_on_custom_cb)(const SignalConfig& cfg);
typedef void (*ui_on_invert_cb)(bool inverted);

// Initialize LVGL and create UI page. Provide callbacks for actions.
// Returns false if display/touch/LVGL cannot be initialized.
bool ui_init(ui_on_rpm_cb on_rpm, ui_on_pattern_cb on_pattern, ui_on_run_cb on_run, ui_on_custom_cb on_custom, ui_on_invert_cb on_invert);

bool ui_is_ready();

// Backend -> UI synchronization (thread-safe; applied on next ui_task_handler())
void ui_update_rpm(uint32_t rpm);
void ui_update_pattern(uint8_t pattern_index);
void ui_update_running(bool running);
void ui_update_inverted(bool inverted);
void ui_show_error(const char* msg);

// Pump LVGL (call often from loop)
void ui_task_handler();
