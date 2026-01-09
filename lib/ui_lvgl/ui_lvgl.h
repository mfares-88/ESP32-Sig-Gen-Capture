// Minimal LVGL UI wrapper for ESP32-S3
#pragma once

#include <lvgl.h>
#include <stdint.h>

typedef void (*ui_on_rpm_cb)(uint32_t rpm);
typedef void (*ui_on_pattern_cb)(uint8_t pattern_index);

// Initialize LVGL and create UI page. Provide callbacks for actions.
void ui_init(ui_on_rpm_cb on_rpm, ui_on_pattern_cb on_pattern);

// Pump LVGL (call often from loop)
void ui_task_handler();
