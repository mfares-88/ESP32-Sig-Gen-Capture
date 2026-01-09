// LVGL v9 UI + display/touch port for JC4827W543 (ESP32-S3)
#include "ui_lvgl.h"

#include <Arduino.h>
#include <Wire.h>
#include <esp_heap_caps.h>
#include <Arduino_GFX_Library.h>
#include <PINS_JC4827W543.h>
#include "TAMC_GT911.h"

// Touch controller configuration (GT911)
static constexpr uint8_t  kTouchSda = I2C_SDA;
static constexpr uint8_t  kTouchScl = I2C_SCL;
static constexpr uint8_t  kTouchInt = TOUCH_INT;
static constexpr uint8_t  kTouchRst = TOUCH_RES;
static constexpr uint16_t kPanelWidth = 480;
static constexpr uint16_t kPanelHeight = 272;
static constexpr uint8_t  kTouchRotation = ROTATION_INVERTED;
static constexpr uint16_t kTouchWidth = kPanelWidth;
static constexpr uint16_t kTouchHeight = kPanelHeight;
static constexpr bool kArcReverse = false;

TAMC_GT911 touchController(kTouchSda, kTouchScl, kTouchInt, kTouchRst, kTouchWidth, kTouchHeight);

// ---- Callbacks provided by application ----
static ui_on_rpm_cb     s_on_rpm = nullptr;
static ui_on_pattern_cb s_on_pattern = nullptr;

// ---- LVGL objects ----
static lv_obj_t* screen_main = nullptr;
static lv_obj_t* arc_rpm = nullptr;
static lv_obj_t* lbl_rpm_value = nullptr;
static lv_obj_t* lbl_rpm_caption = nullptr;
static lv_obj_t* lbl_title = nullptr;
static lv_obj_t* lbl_pattern = nullptr;
static lv_obj_t* dd_patterns = nullptr;

// ---- LVGL display state ----
static lv_display_t* s_disp = nullptr;
static lv_indev_t* s_indev = nullptr;
static lv_color_t* s_draw_buf = nullptr;
static uint32_t s_draw_buf_px = 0;
static uint32_t s_screen_w = 480;
static uint32_t s_screen_h = 272;
static bool s_lvgl_ready = false;

// ---- Styles ----
static lv_style_t style_bg;
static lv_style_t style_title;
static lv_style_t style_caption;
static lv_style_t style_value;
static lv_style_t style_arc_main;
static lv_style_t style_arc_indic;
static lv_style_t style_dropdown;

static uint8_t pick_display_rotation();
static void init_styles();
static void create_main_screen();
static void on_arc_changed(lv_event_t* e);
static void on_pattern_changed(lv_event_t* e);
static void on_pattern_open(lv_event_t* e);
static void update_rpm_label(int32_t rpm);

// ---- LVGL callbacks ----
static void my_print(lv_log_level_t level, const char* buf) {
  LV_UNUSED(level);
  Serial.println(buf);
  Serial.flush();
}

static uint32_t millis_cb(void) {
  return millis();
}

static void my_disp_flush(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
  uint32_t w = lv_area_get_width(area);
  uint32_t h = lv_area_get_height(area);

  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t*)px_map, w, h);
  lv_disp_flush_ready(disp);
}

static void my_touchpad_read(lv_indev_t* indev, lv_indev_data_t* data) {
  LV_UNUSED(indev);

  touchController.read();
  if (touchController.isTouched && touchController.touches > 0) {
    data->point.x = touchController.points[0].x;
    data->point.y = touchController.points[0].y;
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

static uint8_t pick_display_rotation() {
  const uint8_t candidates[] = {0, 1, 2, 3};
  for (uint8_t i = 0; i < sizeof(candidates); ++i) {
    gfx->setRotation(candidates[i]);
    if (gfx->width() == kPanelWidth && gfx->height() == kPanelHeight) return candidates[i];
  }
  for (uint8_t i = 0; i < sizeof(candidates); ++i) {
    gfx->setRotation(candidates[i]);
    if (gfx->width() > gfx->height()) return candidates[i];
  }
  gfx->setRotation(0);
  return 0;
}

static void init_styles() {
  static bool inited = false;
  if (inited) return;
  inited = true;

  lv_style_init(&style_bg);
  lv_style_set_bg_color(&style_bg, lv_color_hex(0x0B1020));
  lv_style_set_bg_grad_color(&style_bg, lv_color_hex(0x141C2E));
  lv_style_set_bg_grad_dir(&style_bg, LV_GRAD_DIR_VER);
  lv_style_set_bg_opa(&style_bg, LV_OPA_COVER);

  lv_style_init(&style_title);
  lv_style_set_text_color(&style_title, lv_color_hex(0xD7E9FF));
  lv_style_set_text_letter_space(&style_title, 2);
  lv_style_set_text_font(&style_title, &lv_font_montserrat_14);

  lv_style_init(&style_caption);
  lv_style_set_text_color(&style_caption, lv_color_hex(0x7C8DB0));
  lv_style_set_text_font(&style_caption, &lv_font_montserrat_10);

  lv_style_init(&style_value);
  lv_style_set_text_color(&style_value, lv_color_hex(0x00E5FF));
  lv_style_set_text_font(&style_value, &lv_font_montserrat_14);

  lv_style_init(&style_arc_main);
  lv_style_set_arc_width(&style_arc_main, 18);
  lv_style_set_arc_color(&style_arc_main, lv_color_hex(0x1B2438));
  lv_style_set_arc_rounded(&style_arc_main, true);

  lv_style_init(&style_arc_indic);
  lv_style_set_arc_width(&style_arc_indic, 18);
  lv_style_set_arc_color(&style_arc_indic, lv_color_hex(0x00E5FF));
  lv_style_set_arc_rounded(&style_arc_indic, true);
  lv_style_set_shadow_width(&style_arc_indic, 18);
  lv_style_set_shadow_color(&style_arc_indic, lv_color_hex(0x00E5FF));
  lv_style_set_shadow_opa(&style_arc_indic, LV_OPA_40);
  lv_style_set_shadow_spread(&style_arc_indic, 2);

  lv_style_init(&style_dropdown);
  lv_style_set_bg_color(&style_dropdown, lv_color_hex(0x0F1628));
  lv_style_set_bg_grad_color(&style_dropdown, lv_color_hex(0x101F34));
  lv_style_set_bg_grad_dir(&style_dropdown, LV_GRAD_DIR_VER);
  lv_style_set_bg_opa(&style_dropdown, LV_OPA_COVER);
  lv_style_set_border_color(&style_dropdown, lv_color_hex(0x00E5FF));
  lv_style_set_border_width(&style_dropdown, 1);
  lv_style_set_border_opa(&style_dropdown, LV_OPA_50);
  lv_style_set_text_color(&style_dropdown, lv_color_hex(0xD7E9FF));
  lv_style_set_pad_all(&style_dropdown, 6);
  lv_style_set_shadow_width(&style_dropdown, 10);
  lv_style_set_shadow_color(&style_dropdown, lv_color_hex(0x00E5FF));
  lv_style_set_shadow_opa(&style_dropdown, LV_OPA_20);
}

static void create_main_screen() {
  screen_main = lv_screen_active();
  lv_obj_remove_style_all(screen_main);
  lv_obj_add_style(screen_main, &style_bg, 0);

  lbl_title = lv_label_create(screen_main);
  lv_label_set_text(lbl_title, "CKP SIGNAL");
  lv_obj_add_style(lbl_title, &style_title, 0);
  lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 10);

  arc_rpm = lv_arc_create(screen_main);
  lv_obj_set_size(arc_rpm, 200, 200);
  lv_arc_set_rotation(arc_rpm, 135);
  lv_arc_set_bg_angles(arc_rpm, 0, 270);
  lv_arc_set_mode(arc_rpm, kArcReverse ? LV_ARC_MODE_REVERSE : LV_ARC_MODE_NORMAL);
  lv_arc_set_range(arc_rpm, 100, 6000);
  lv_arc_set_value(arc_rpm, 1000);
  lv_obj_add_style(arc_rpm, &style_arc_main, LV_PART_MAIN);
  lv_obj_add_style(arc_rpm, &style_arc_indic, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(arc_rpm, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_set_style_border_width(arc_rpm, 0, LV_PART_KNOB);
  lv_obj_add_flag(arc_rpm, LV_OBJ_FLAG_ADV_HITTEST);
  lv_obj_align(arc_rpm, LV_ALIGN_TOP_LEFT, 20, 24);
  lv_obj_add_event_cb(arc_rpm, on_arc_changed, LV_EVENT_VALUE_CHANGED, NULL);

  lbl_rpm_value = lv_label_create(screen_main);
  lv_obj_add_style(lbl_rpm_value, &style_value, 0);
  lv_label_set_text(lbl_rpm_value, "1000");
  lv_obj_align_to(lbl_rpm_value, arc_rpm, LV_ALIGN_CENTER, 0, -6);

  lbl_rpm_caption = lv_label_create(screen_main);
  lv_obj_add_style(lbl_rpm_caption, &style_caption, 0);
  lv_label_set_text(lbl_rpm_caption, "RPM");
  lv_obj_align_to(lbl_rpm_caption, arc_rpm, LV_ALIGN_CENTER, 0, 16);

  lbl_pattern = lv_label_create(screen_main);
  lv_obj_add_style(lbl_pattern, &style_caption, 0);
  lv_label_set_text(lbl_pattern, "PATTERN");

  dd_patterns = lv_dropdown_create(screen_main);
  lv_dropdown_set_options(dd_patterns, "60-2\n36-1\n36-2\n36-1-1");
  lv_obj_set_width(dd_patterns, 160);
  lv_obj_add_style(dd_patterns, &style_dropdown, LV_PART_MAIN);
  lv_obj_t* list = lv_dropdown_get_list(dd_patterns);
  if (list) {
    lv_obj_add_style(list, &style_dropdown, LV_PART_MAIN);
  }
  lv_obj_align(dd_patterns, LV_ALIGN_TOP_RIGHT, -20, 24);
  lv_obj_align_to(lbl_pattern, dd_patterns, LV_ALIGN_OUT_TOP_MID, 0, -6);
  lv_obj_add_event_cb(dd_patterns, on_pattern_changed, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(dd_patterns, on_pattern_open, LV_EVENT_CLICKED, NULL);

  update_rpm_label(lv_arc_get_value(arc_rpm));
}

static void update_rpm_label(int32_t rpm) {
  if (!lbl_rpm_value) return;
  lv_label_set_text_fmt(lbl_rpm_value, "%ld", (long)rpm);
}

static void on_arc_changed(lv_event_t* e) {
  lv_obj_t* arc = lv_event_get_target_obj(e);
  int32_t rpm = lv_arc_get_value(arc);
  update_rpm_label(rpm);
  if (s_on_rpm) s_on_rpm((uint32_t)rpm);
}

static void on_pattern_changed(lv_event_t* e) {
  lv_obj_t* dd = lv_event_get_target_obj(e);
  uint16_t sel = lv_dropdown_get_selected(dd);
  if (s_on_pattern) s_on_pattern((uint8_t)sel);
}

static void on_pattern_open(lv_event_t* e) {
  lv_obj_t* dd = lv_event_get_target_obj(e);
  lv_obj_t* list = lv_dropdown_get_list(dd);
  if (list) lv_obj_move_foreground(list);
}

void ui_init(ui_on_rpm_cb on_rpm, ui_on_pattern_cb on_pattern) {
  s_on_rpm = on_rpm;
  s_on_pattern = on_pattern;
  if (s_lvgl_ready) return;

  if (!gfx->begin()) {
    Serial.println("gfx->begin() failed!");
    while (true) { delay(100); }
  }

  const uint8_t kDisplayRotation = pick_display_rotation();

  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
  gfx->setRotation(kDisplayRotation);
  gfx->fillScreen(RGB565_BLACK);

  Wire.begin(kTouchSda, kTouchScl);
  touchController.begin();
  touchController.setRotation(kTouchRotation);

  lv_init();
  lv_tick_set_cb(millis_cb);

#if LV_USE_LOG != 0
  lv_log_register_print_cb(my_print);
#endif

  s_screen_w = gfx->width();
  s_screen_h = gfx->height();
  touchController.setResolution(s_screen_w, s_screen_h);
  s_draw_buf_px = s_screen_w * 40;
  size_t buf_bytes = s_draw_buf_px * sizeof(lv_color_t);

  s_draw_buf = static_cast<lv_color_t*>(heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!s_draw_buf) {
    s_draw_buf = static_cast<lv_color_t*>(heap_caps_malloc(buf_bytes, MALLOC_CAP_8BIT));
  }
  if (!s_draw_buf) {
    Serial.println("LVGL draw buffer alloc failed!");
    while (true) { delay(100); }
  }

  s_disp = lv_display_create(s_screen_w, s_screen_h);
  lv_display_set_flush_cb(s_disp, my_disp_flush);
  lv_display_set_buffers(s_disp, s_draw_buf, NULL, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

  s_indev = lv_indev_create();
  lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(s_indev, my_touchpad_read);
  lv_indev_set_display(s_indev, s_disp);

  init_styles();
  create_main_screen();
  s_lvgl_ready = true;
}

void ui_task_handler() {
  if (!s_lvgl_ready) return;
  lv_timer_handler();
}
