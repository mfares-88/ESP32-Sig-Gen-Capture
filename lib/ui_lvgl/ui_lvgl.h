// Minimal LVGL UI wrapper for ESP32-S3
#pragma once

#include <lvgl.h>
#include "CkpGenerator.h"  // for SignalConfig / GapPosition

typedef void (*ui_on_pattern_cb)(uint8_t pattern_index);
typedef void (*ui_on_custom_cb)(const SignalConfig* cfg);

// Initialize LVGL and create UI pages. Provide callbacks for actions.
void ui_init(ui_on_pattern_cb on_pattern, ui_on_custom_cb on_custom);

// Pump LVGL (call often from loop)
void ui_task_handler();
