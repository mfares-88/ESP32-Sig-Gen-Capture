// LVGL v9 UI + display/touch port for JC4827W543 (ESP32-S3)
#include "ui_lvgl.h"

#include <Arduino.h>
#include <Wire.h>
#include <esp_heap_caps.h>
#include <Arduino_GFX_Library.h>
#include <PINS_JC4827W543.h>
#include "TAMC_GT911.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include <string.h>


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
static ui_on_run_cb     s_on_run = nullptr;
static ui_on_custom_cb  s_on_custom = nullptr;


// ---- LVGL objects ----
static lv_obj_t* screen_main = nullptr;
static lv_obj_t* arc_rpm = nullptr;
static lv_obj_t* lbl_rpm_value = nullptr;
static lv_obj_t* lbl_rpm_caption = nullptr;
static lv_obj_t* lbl_title = nullptr;
static lv_obj_t* lbl_pattern = nullptr;
static lv_obj_t* dd_patterns = nullptr;
static lv_obj_t* btn_run = nullptr;
static lv_obj_t* lbl_run = nullptr;
static lv_obj_t* lbl_error = nullptr;

// ---- Custom pattern modal ----
static lv_obj_t* overlay_custom = nullptr;
static lv_obj_t* panel_custom = nullptr;
static lv_obj_t* spin_teeth = nullptr;
static lv_obj_t* spin_pmiss = nullptr;
static lv_obj_t* spin_nmiss = nullptr;
static lv_obj_t* dd_gap_pos = nullptr;
static lv_obj_t* sw_gap_lvl = nullptr;
static lv_obj_t* lbl_custom_error = nullptr;
static uint8_t s_last_preset_pattern = 0;


// ---- LVGL display state ----
static lv_display_t* s_disp = nullptr;
static lv_indev_t* s_indev = nullptr;
static lv_color_t* s_draw_buf = nullptr;
static uint32_t s_draw_buf_px = 0;
static uint32_t s_screen_w = 480;
static uint32_t s_screen_h = 272;
static bool s_lvgl_ready = false;

static portMUX_TYPE s_ui_mux = portMUX_INITIALIZER_UNLOCKED;

static volatile bool s_pending_rpm = false;
static volatile uint32_t s_pending_rpm_val = 0;
static volatile bool s_pending_pattern = false;
static volatile uint8_t s_pending_pattern_val = 0;
static volatile bool s_pending_running = false;
static volatile bool s_pending_running_val = false;
static volatile bool s_pending_error = false;
static char s_pending_error_msg[96];

static bool s_suppress_rpm_cb = false;
static bool s_suppress_pattern_cb = false;
static bool s_suppress_run_cb = false;

static bool s_running = true;
static uint32_t s_rpm_flash_until_ms = 0;


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
static void on_run_clicked(lv_event_t* e);
static void update_rpm_label(int32_t rpm);
static void apply_pending_updates();

static SignalConfig presetCfgFromIndex(uint8_t idx, uint32_t rpm);
static void open_custom_panel();
static void close_custom_panel();
static void set_custom_error(const char* msg);
static void on_spin_inc(lv_event_t* e);
static void on_spin_dec(lv_event_t* e);
static void on_custom_apply(lv_event_t* e);
static void on_custom_cancel(lv_event_t* e);


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

  static uint32_t last_ms = 0;
  static lv_point_t last_pt{0, 0};
  static bool last_pressed = false;

  const uint32_t now_ms = millis();

  touchController.read();
  const bool pressed = (touchController.isTouched && touchController.touches > 0);
  if (!pressed) {
    data->state = LV_INDEV_STATE_RELEASED;
    last_pressed = false;
    return;
  }

  const lv_point_t pt{(lv_coord_t)touchController.points[0].x, (lv_coord_t)touchController.points[0].y};

  if (last_pressed && (now_ms - last_ms) < 50) {
    const int dx = (int)pt.x - (int)last_pt.x;
    const int dy = (int)pt.y - (int)last_pt.y;
    if ((dx * dx + dy * dy) < 9) {
      data->point = last_pt;
      data->state = LV_INDEV_STATE_PRESSED;
      return;
    }
  }

  last_ms = now_ms;
  last_pt = pt;
  last_pressed = true;

  data->point = pt;
  data->state = LV_INDEV_STATE_PRESSED;
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

static SignalConfig presetCfgFromIndex(uint8_t idx, uint32_t rpm) {
  SignalConfig c{rpm, 60, 1, 2, GAP_AT_END, false};
  switch (idx) {
    case 0: c = {rpm, 60, 1, 2, GAP_AT_END, false}; break;
    case 1: c = {rpm, 36, 1, 1, GAP_AT_END, false}; break;
    case 2: c = {rpm, 36, 1, 2, GAP_AT_END, false}; break;
    case 3: c = {rpm, 36, 2, 1, GAP_AT_END, false}; break;
    case 4: c = {rpm, 12, 1, 1, GAP_AT_START, true}; break;
  }
  return c;
}

static void set_custom_error(const char* msg) {
  if (!lbl_custom_error) return;
  if (!msg || msg[0] == '\0') {
    lv_obj_add_flag(lbl_custom_error, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_label_set_text(lbl_custom_error, msg);
  lv_obj_clear_flag(lbl_custom_error, LV_OBJ_FLAG_HIDDEN);
}

static void on_spin_inc(lv_event_t* e) {
  lv_obj_t* spin = (lv_obj_t*)lv_event_get_user_data(e);
  if (!spin) return;
  lv_spinbox_increment(spin);
}

static void on_spin_dec(lv_event_t* e) {
  lv_obj_t* spin = (lv_obj_t*)lv_event_get_user_data(e);
  if (!spin) return;
  lv_spinbox_decrement(spin);
}

static lv_obj_t* make_spin_row(lv_obj_t* parent, const char* caption, lv_obj_t** out_spin, int32_t min, int32_t max, int32_t initial) {
  lv_obj_t* row = lv_obj_create(parent);
  lv_obj_set_size(row, lv_pct(100), 34);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 0, 0);

  lv_obj_t* lbl = lv_label_create(row);
  lv_label_set_text(lbl, caption);
  lv_obj_add_style(lbl, &style_caption, 0);
  lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

  lv_obj_t* spin = lv_spinbox_create(row);
  lv_spinbox_set_range(spin, min, max);
  lv_spinbox_set_value(spin, initial);
  lv_spinbox_set_digit_format(spin, 4, 0);
  lv_obj_set_width(spin, 70);
  lv_obj_align(spin, LV_ALIGN_RIGHT_MID, -40, 0);

  lv_obj_t* btn_minus = lv_btn_create(row);
  lv_obj_set_size(btn_minus, 32, 30);
  lv_obj_align(btn_minus, LV_ALIGN_RIGHT_MID, -84, 0);
  lv_obj_add_event_cb(btn_minus, on_spin_dec, LV_EVENT_CLICKED, spin);

  lv_obj_t* lbl_minus = lv_label_create(btn_minus);
  lv_label_set_text(lbl_minus, "-");
  lv_obj_center(lbl_minus);

  lv_obj_t* btn_plus = lv_btn_create(row);
  lv_obj_set_size(btn_plus, 32, 30);
  lv_obj_align(btn_plus, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_add_event_cb(btn_plus, on_spin_inc, LV_EVENT_CLICKED, spin);

  lv_obj_t* lbl_plus = lv_label_create(btn_plus);
  lv_label_set_text(lbl_plus, "+");
  lv_obj_center(lbl_plus);

  if (out_spin) *out_spin = spin;
  return row;
}

static void close_custom_panel() {
  if (overlay_custom) {
    lv_obj_del(overlay_custom);
  }
  overlay_custom = nullptr;
  panel_custom = nullptr;
  spin_teeth = nullptr;
  spin_pmiss = nullptr;
  spin_nmiss = nullptr;
  dd_gap_pos = nullptr;
  sw_gap_lvl = nullptr;
  lbl_custom_error = nullptr;
}

static void on_custom_cancel(lv_event_t* e) {
  LV_UNUSED(e);
  close_custom_panel();
}

static void on_custom_apply(lv_event_t* e) {
  LV_UNUSED(e);
  if (!spin_teeth || !spin_pmiss || !spin_nmiss || !dd_gap_pos || !sw_gap_lvl) return;

  const uint32_t rpm = arc_rpm ? (uint32_t)lv_arc_get_value(arc_rpm) : 1000u;

  SignalConfig cfg{};
  cfg.rpm = rpm;
  cfg.nTeeth = (uint16_t)lv_spinbox_get_value(spin_teeth);
  cfg.pMiss = (uint8_t)lv_spinbox_get_value(spin_pmiss);
  cfg.nMiss = (uint8_t)lv_spinbox_get_value(spin_nmiss);

  const uint16_t posSel = lv_dropdown_get_selected(dd_gap_pos);
  cfg.gapPos = (posSel == 1) ? GAP_AT_START : GAP_AT_END;
  cfg.gapLvl = lv_obj_has_state(sw_gap_lvl, LV_STATE_CHECKED);

  if (!validateSignalConfig(cfg)) {
    set_custom_error("Invalid combination");
    return;
  }

  if (!s_on_custom) {
    set_custom_error("Custom callback missing");
    return;
  }

  ui_show_error("");
  set_custom_error("");
  close_custom_panel();
  s_on_custom(cfg);
}

static void open_custom_panel() {
  if (!screen_main) return;
  if (overlay_custom) return;

  overlay_custom = lv_obj_create(screen_main);
  lv_obj_set_size(overlay_custom, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_color(overlay_custom, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(overlay_custom, LV_OPA_70, 0);
  lv_obj_set_style_border_width(overlay_custom, 0, 0);
  lv_obj_set_style_pad_all(overlay_custom, 0, 0);
  lv_obj_clear_flag(overlay_custom, LV_OBJ_FLAG_SCROLLABLE);

  panel_custom = lv_obj_create(overlay_custom);
  lv_obj_set_size(panel_custom, 380, 240);
  lv_obj_center(panel_custom);
  lv_obj_add_style(panel_custom, &style_dropdown, 0);
  lv_obj_set_style_pad_all(panel_custom, 10, 0);

  lv_obj_t* title = lv_label_create(panel_custom);
  lv_label_set_text(title, "CUSTOM PATTERN");
  lv_obj_add_style(title, &style_title, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

  lv_obj_t* hint = lv_label_create(panel_custom);
  lv_label_set_text(hint, "RPM uses the dial on the left");
  lv_obj_add_style(hint, &style_caption, 0);
  lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 18);

  const uint32_t rpm = arc_rpm ? (uint32_t)lv_arc_get_value(arc_rpm) : 1000u;
  const SignalConfig seed = presetCfgFromIndex(s_last_preset_pattern, rpm);

  lv_obj_t* rows = lv_obj_create(panel_custom);
  lv_obj_set_size(rows, lv_pct(100), 140);
  lv_obj_align(rows, LV_ALIGN_TOP_MID, 0, 40);
  lv_obj_set_style_bg_opa(rows, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(rows, 0, 0);
  lv_obj_set_style_pad_all(rows, 0, 0);
  lv_obj_set_style_pad_row(rows, 6, 0);
  lv_obj_set_flex_flow(rows, LV_FLEX_FLOW_COLUMN);
  lv_obj_clear_flag(rows, LV_OBJ_FLAG_SCROLLABLE);

  (void)make_spin_row(rows, "Teeth", &spin_teeth, 1, 120, seed.nTeeth);
  (void)make_spin_row(rows, "Periods/Rev", &spin_pmiss, 1, 10, seed.pMiss);
  (void)make_spin_row(rows, "Missing/Period", &spin_nmiss, 1, 60, seed.nMiss);

  lv_obj_t* rowPos = lv_obj_create(rows);
  lv_obj_set_size(rowPos, lv_pct(100), 34);
  lv_obj_set_style_bg_opa(rowPos, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(rowPos, 0, 0);
  lv_obj_set_style_pad_all(rowPos, 0, 0);

  lv_obj_t* lblPos = lv_label_create(rowPos);
  lv_label_set_text(lblPos, "Gap Pos");
  lv_obj_add_style(lblPos, &style_caption, 0);
  lv_obj_align(lblPos, LV_ALIGN_LEFT_MID, 0, 0);

  dd_gap_pos = lv_dropdown_create(rowPos);
  lv_dropdown_set_options(dd_gap_pos, "END\nSTART");
  lv_obj_set_width(dd_gap_pos, 140);
  lv_obj_align(dd_gap_pos, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_add_style(dd_gap_pos, &style_dropdown, LV_PART_MAIN);
  lv_dropdown_set_selected(dd_gap_pos, (seed.gapPos == GAP_AT_START) ? 1 : 0);

  lv_obj_t* rowLvl = lv_obj_create(rows);
  lv_obj_set_size(rowLvl, lv_pct(100), 34);
  lv_obj_set_style_bg_opa(rowLvl, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(rowLvl, 0, 0);
  lv_obj_set_style_pad_all(rowLvl, 0, 0);

  lv_obj_t* lblLvl = lv_label_create(rowLvl);
  lv_label_set_text(lblLvl, "Gap HIGH");
  lv_obj_add_style(lblLvl, &style_caption, 0);
  lv_obj_align(lblLvl, LV_ALIGN_LEFT_MID, 0, 0);

  sw_gap_lvl = lv_switch_create(rowLvl);
  lv_obj_align(sw_gap_lvl, LV_ALIGN_RIGHT_MID, 0, 0);
  if (seed.gapLvl) lv_obj_add_state(sw_gap_lvl, LV_STATE_CHECKED);

  lbl_custom_error = lv_label_create(panel_custom);
  lv_obj_add_style(lbl_custom_error, &style_caption, 0);
  lv_label_set_text(lbl_custom_error, "");
  lv_obj_align(lbl_custom_error, LV_ALIGN_BOTTOM_LEFT, 0, -48);
  lv_obj_add_flag(lbl_custom_error, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t* btnCancel = lv_btn_create(panel_custom);
  lv_obj_set_size(btnCancel, 120, 38);
  lv_obj_align(btnCancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_add_event_cb(btnCancel, on_custom_cancel, LV_EVENT_CLICKED, NULL);
  lv_obj_t* lblCancel = lv_label_create(btnCancel);
  lv_label_set_text(lblCancel, "CANCEL");
  lv_obj_center(lblCancel);

  lv_obj_t* btnApply = lv_btn_create(panel_custom);
  lv_obj_set_size(btnApply, 120, 38);
  lv_obj_align(btnApply, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_add_event_cb(btnApply, on_custom_apply, LV_EVENT_CLICKED, NULL);
  lv_obj_t* lblApply = lv_label_create(btnApply);
  lv_label_set_text(lblApply, "APPLY");
  lv_obj_center(lblApply);
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
  lv_dropdown_set_options(dd_patterns, "60-2\n36-1\n36-2\n36-1-1\n12-1\nCUSTOM");
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

  btn_run = lv_btn_create(screen_main);
  lv_obj_set_size(btn_run, 160, 44);
  lv_obj_align(btn_run, LV_ALIGN_BOTTOM_RIGHT, -20, -18);
  lv_obj_add_event_cb(btn_run, on_run_clicked, LV_EVENT_CLICKED, NULL);

  lbl_run = lv_label_create(btn_run);
  lv_label_set_text(lbl_run, s_running ? "STOP" : "START");
  lv_obj_center(lbl_run);

  lbl_error = lv_label_create(screen_main);
  lv_obj_add_style(lbl_error, &style_caption, 0);
  lv_label_set_text(lbl_error, "");
  lv_obj_align(lbl_error, LV_ALIGN_BOTTOM_LEFT, 12, -10);
  lv_obj_add_flag(lbl_error, LV_OBJ_FLAG_HIDDEN);

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
  if (!s_suppress_rpm_cb && s_on_rpm) s_on_rpm((uint32_t)rpm);

}

static void on_pattern_changed(lv_event_t* e) {
  lv_obj_t* dd = lv_event_get_target_obj(e);
  uint16_t sel = lv_dropdown_get_selected(dd);

  if (s_suppress_pattern_cb) return;

  if (sel == 5) {
    open_custom_panel();
    s_suppress_pattern_cb = true;
    lv_dropdown_set_selected(dd, s_last_preset_pattern);
    s_suppress_pattern_cb = false;
    return;
  }

  s_last_preset_pattern = (uint8_t)sel;
  if (s_on_pattern) s_on_pattern((uint8_t)sel);
}


static void on_pattern_open(lv_event_t* e) {
  lv_obj_t* dd = lv_event_get_target_obj(e);
  lv_obj_t* list = lv_dropdown_get_list(dd);
  if (list) lv_obj_move_foreground(list);
}

static void refresh_run_label() {
  if (!lbl_run) return;
  lv_label_set_text(lbl_run, s_running ? "STOP" : "START");
}

static void on_run_clicked(lv_event_t* e) {
  LV_UNUSED(e);
  if (s_suppress_run_cb) return;

  s_running = !s_running;
  refresh_run_label();

  if (s_on_run) s_on_run(s_running);
}

bool ui_init(ui_on_rpm_cb on_rpm, ui_on_pattern_cb on_pattern, ui_on_run_cb on_run, ui_on_custom_cb on_custom) {
  s_on_rpm = on_rpm;
  s_on_pattern = on_pattern;
  s_on_run = on_run;
  s_on_custom = on_custom;
  if (s_lvgl_ready) return true;


  if (!gfx->begin()) {
    Serial.println("gfx->begin() failed!");
    return false;
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

  const uint8_t row_candidates[] = {40, 20, 10, 5};
  s_draw_buf = nullptr;
  size_t buf_bytes = 0;

  for (uint8_t i = 0; i < sizeof(row_candidates); ++i) {
    s_draw_buf_px = s_screen_w * row_candidates[i];
    buf_bytes = s_draw_buf_px * sizeof(lv_color_t);

    s_draw_buf = static_cast<lv_color_t*>(heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!s_draw_buf) {
      s_draw_buf = static_cast<lv_color_t*>(heap_caps_malloc(buf_bytes, MALLOC_CAP_8BIT));
    }
    if (s_draw_buf) break;
  }

  if (!s_draw_buf) {
    Serial.println("LVGL draw buffer alloc failed!");
    return false;
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
  return true;
}


bool ui_is_ready() {
  return s_lvgl_ready;
}

void ui_update_rpm(uint32_t rpm) {
  portENTER_CRITICAL(&s_ui_mux);
  s_pending_rpm_val = rpm;
  s_pending_rpm = true;
  portEXIT_CRITICAL(&s_ui_mux);
}

void ui_update_pattern(uint8_t pattern_index) {
  portENTER_CRITICAL(&s_ui_mux);
  s_pending_pattern_val = pattern_index;
  s_pending_pattern = true;
  portEXIT_CRITICAL(&s_ui_mux);
}

void ui_update_running(bool running) {
  portENTER_CRITICAL(&s_ui_mux);
  s_pending_running_val = running;
  s_pending_running = true;
  portEXIT_CRITICAL(&s_ui_mux);
}

void ui_show_error(const char* msg) {
  portENTER_CRITICAL(&s_ui_mux);
  strncpy(s_pending_error_msg, msg ? msg : "", sizeof(s_pending_error_msg));
  s_pending_error_msg[sizeof(s_pending_error_msg) - 1] = '\0';
  s_pending_error = true;
  portEXIT_CRITICAL(&s_ui_mux);
}

static void apply_pending_updates() {
  bool hasRpm = false;
  uint32_t rpm = 0;
  bool hasPattern = false;
  uint8_t pattern = 0;
  bool hasRunning = false;
  bool running = false;
  bool hasError = false;
  char errorMsg[sizeof(s_pending_error_msg)];

  portENTER_CRITICAL(&s_ui_mux);
  if (s_pending_rpm) { hasRpm = true; rpm = s_pending_rpm_val; s_pending_rpm = false; }
  if (s_pending_pattern) { hasPattern = true; pattern = s_pending_pattern_val; s_pending_pattern = false; }
  if (s_pending_running) { hasRunning = true; running = s_pending_running_val; s_pending_running = false; }
  if (s_pending_error) {
    hasError = true;
    strncpy(errorMsg, s_pending_error_msg, sizeof(errorMsg));
    errorMsg[sizeof(errorMsg) - 1] = '\0';
    s_pending_error = false;
  }
  portEXIT_CRITICAL(&s_ui_mux);

  if (hasRpm && arc_rpm) {
    s_suppress_rpm_cb = true;
    lv_arc_set_value(arc_rpm, (int32_t)rpm);
    update_rpm_label((int32_t)rpm);
    s_suppress_rpm_cb = false;

    if (lbl_rpm_value) {
      lv_obj_set_style_text_color(lbl_rpm_value, lv_color_hex(0xFFB020), 0);
      s_rpm_flash_until_ms = millis() + 600;
    }
  }

  if (hasPattern && dd_patterns) {
    if (pattern < 5) {
      s_last_preset_pattern = pattern;
      s_suppress_pattern_cb = true;
      lv_dropdown_set_selected(dd_patterns, pattern);
      s_suppress_pattern_cb = false;
    }
  }

  if (hasRunning) {
    s_suppress_run_cb = true;
    s_running = running;
    refresh_run_label();
    s_suppress_run_cb = false;
  }

  if (hasError && lbl_error) {
    if (errorMsg[0] == '\0') {
      lv_obj_add_flag(lbl_error, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_label_set_text(lbl_error, errorMsg);
      lv_obj_clear_flag(lbl_error, LV_OBJ_FLAG_HIDDEN);
    }
  }

  if (lbl_rpm_value && s_rpm_flash_until_ms != 0 && (int32_t)(millis() - s_rpm_flash_until_ms) >= 0) {
    lv_obj_set_style_text_color(lbl_rpm_value, lv_color_hex(0x00E5FF), 0);
    s_rpm_flash_until_ms = 0;
  }
}

void ui_task_handler() {
  if (!s_lvgl_ready) return;
  lv_timer_handler();
  apply_pending_updates();
}

