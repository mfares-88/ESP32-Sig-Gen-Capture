// Minimal LVGL UI implementation with two screens:
//  - Main screen: dropdown for preset patterns + Custom button
//  - Custom screen: text boxes for rpm, teeth, pmiss, nmiss, pos, lvl and Apply/Back

#include "ui_lvgl.h"

// ---- Callbacks provided by application ----
static ui_on_pattern_cb s_on_pattern = nullptr;
static ui_on_custom_cb  s_on_custom  = nullptr;

// ---- LVGL objects ----
static lv_obj_t* screen_main = nullptr;
static lv_obj_t* screen_custom = nullptr;
static lv_obj_t* dd_patterns = nullptr;
static lv_obj_t* btn_custom = nullptr;
static lv_obj_t* lbl_status = nullptr;

// Custom page widgets
static lv_obj_t* ta_rpm = nullptr;
static lv_obj_t* ta_teeth = nullptr;
static lv_obj_t* ta_pmiss = nullptr;
static lv_obj_t* ta_nmiss = nullptr;
static lv_obj_t* ta_pos = nullptr;   // accepts 's' or 'e'
static lv_obj_t* ta_lvl = nullptr;   // accepts 'h' or 'l'
static lv_obj_t* btn_apply = nullptr;
static lv_obj_t* btn_back = nullptr;

// ---- Minimal display driver (stub) ----
// This registers a dummy flush callback so LVGL can run even without a real panel driver.
// Replace with your actual panel driver later.
static void disp_flush_stub(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
  LV_UNUSED(drv); LV_UNUSED(area); LV_UNUSED(color_p);
  lv_disp_flush_ready(drv);
}

static void register_display_stub() {
  static lv_color_t buf1[320 * 10]; // small line buffer (assumes 320 width)
  static lv_disp_draw_buf_t draw_buf;
  lv_disp_draw_buf_init(&draw_buf, buf1, NULL, sizeof(buf1)/sizeof(buf1[0]));

  lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = 320;
  disp_drv.ver_res = 240;
  disp_drv.flush_cb = disp_flush_stub;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);
}

// ---- Helpers ----
static void show_custom_page(lv_event_t* e);
static void show_main_page(lv_event_t* e);
static void on_pattern_changed(lv_event_t* e);
static void on_apply_custom(lv_event_t* e);

static void create_main_screen() {
  screen_main = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(screen_main, lv_color_black(), 0);

  lv_obj_t* title = lv_label_create(screen_main);
  lv_label_set_text(title, "Crank Pattern");
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

  // Dropdown for 4 preset patterns
  dd_patterns = lv_dropdown_create(screen_main);
  lv_dropdown_set_options_static(dd_patterns, "60-2\n36-1\n36-2\n36-1-1");
  lv_obj_set_width(dd_patterns, 160);
  lv_obj_align(dd_patterns, LV_ALIGN_TOP_MID, 0, 40);
  lv_obj_add_event_cb(dd_patterns, on_pattern_changed, LV_EVENT_VALUE_CHANGED, NULL);

  // Custom button
  btn_custom = lv_btn_create(screen_main);
  lv_obj_align(btn_custom, LV_ALIGN_TOP_MID, 0, 90);
  lv_obj_add_event_cb(btn_custom, show_custom_page, LV_EVENT_CLICKED, NULL);
  lv_obj_t* lbl_c = lv_label_create(btn_custom);
  lv_label_set_text(lbl_c, "Custom...");
  lv_obj_center(lbl_c);

  // Status label
  lbl_status = lv_label_create(screen_main);
  lv_obj_set_style_text_color(lbl_status, lv_color_white(), 0);
  lv_label_set_text(lbl_status, "Select a preset or Custom");
  lv_obj_align(lbl_status, LV_ALIGN_TOP_MID, 0, 130);
}

static lv_obj_t* make_labeled_ta(lv_obj_t* parent, const char* label, const char* placeholder, lv_coord_t y, const char* accepted_chars) {
  lv_obj_t* cont = lv_obj_create(parent);
  lv_obj_remove_style_all(cont);
  lv_obj_set_size(cont, 280, 48);
  lv_obj_align(cont, LV_ALIGN_TOP_MID, 0, y);

  lv_obj_t* l = lv_label_create(cont);
  lv_label_set_text(l, label);
  lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);

  lv_obj_t* ta = lv_textarea_create(cont);
  lv_obj_set_size(ta, 180, 36);
  lv_obj_align(ta, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_textarea_set_placeholder_text(ta, placeholder);
  if (accepted_chars) lv_textarea_set_accepted_chars(ta, accepted_chars);
  return ta;
}

static void create_custom_screen() {
  screen_custom = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(screen_custom, lv_color_black(), 0);

  lv_obj_t* title = lv_label_create(screen_custom);
  lv_label_set_text(title, "Custom Pattern");
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

  ta_rpm   = make_labeled_ta(screen_custom, "RPM",    "100-6000", 30,  "0123456789");
  ta_teeth = make_labeled_ta(screen_custom, "Teeth",  "e.g. 60",   78,  "0123456789");
  ta_pmiss = make_labeled_ta(screen_custom, "Periods","1 or 2",    126, "0123456789");
  ta_nmiss = make_labeled_ta(screen_custom, "Missing","1 or 2",    174, "0123456789");
  ta_pos   = make_labeled_ta(screen_custom, "Pos",    "s/e",       222, "seSE");
  ta_lvl   = make_labeled_ta(screen_custom, "Level",  "h/l",       270, "hlHL");

  btn_back = lv_btn_create(screen_custom);
  lv_obj_set_size(btn_back, 110, 40);
  lv_obj_align(btn_back, LV_ALIGN_BOTTOM_LEFT, 20, -12);
  lv_obj_add_event_cb(btn_back, show_main_page, LV_EVENT_CLICKED, NULL);
  lv_obj_t* lblb = lv_label_create(btn_back);
  lv_label_set_text(lblb, "Back");
  lv_obj_center(lblb);

  btn_apply = lv_btn_create(screen_custom);
  lv_obj_set_size(btn_apply, 110, 40);
  lv_obj_align(btn_apply, LV_ALIGN_BOTTOM_RIGHT, -20, -12);
  lv_obj_add_event_cb(btn_apply, on_apply_custom, LV_EVENT_CLICKED, NULL);
  lv_obj_t* lbla = lv_label_create(btn_apply);
  lv_label_set_text(lbla, "Apply");
  lv_obj_center(lbla);
}

static void show_custom_page(lv_event_t* e) {
  LV_UNUSED(e);
  if (!screen_custom) create_custom_screen();
  lv_scr_load(screen_custom);
}
static void show_main_page(lv_event_t* e) {
  LV_UNUSED(e);
  if (!screen_main) create_main_screen();
  lv_scr_load(screen_main);
}

static void on_pattern_changed(lv_event_t* e) {
  if (!s_on_pattern) return;
  uint16_t sel = lv_dropdown_get_selected(dd_patterns);
  if (lbl_status) {
    const char* txt = lv_dropdown_get_options(dd_patterns);
    // Show simple status
    lv_label_set_text_fmt(lbl_status, "Preset %u selected", (unsigned)sel);
  }
  s_on_pattern((uint8_t)sel);
}

static inline uint32_t parse_u32_or(const char* s, uint32_t fallback) {
  if (!s || !*s) return fallback;
  uint32_t v = (uint32_t)atoi(s);
  return v;
}
static void on_apply_custom(lv_event_t* e) {
  LV_UNUSED(e);
  if (!s_on_custom) return;

  const char* s_rpm   = lv_textarea_get_text(ta_rpm);
  const char* s_teeth = lv_textarea_get_text(ta_teeth);
  const char* s_pmiss = lv_textarea_get_text(ta_pmiss);
  const char* s_nmiss = lv_textarea_get_text(ta_nmiss);
  const char* s_pos   = lv_textarea_get_text(ta_pos);
  const char* s_lvl   = lv_textarea_get_text(ta_lvl);

  SignalConfig cfg{};
  cfg.rpm    = parse_u32_or(s_rpm,   1000);
  cfg.nTeeth = (uint16_t)parse_u32_or(s_teeth, 60);
  cfg.pMiss  = (uint8_t) parse_u32_or(s_pmiss, 1);
  cfg.nMiss  = (uint8_t) parse_u32_or(s_nmiss, 2);
  cfg.gapPos = (s_pos && (s_pos[0] == 's' || s_pos[0] == 'S')) ? GAP_AT_START : GAP_AT_END;
  cfg.gapLvl = (s_lvl && (s_lvl[0] == 'h' || s_lvl[0] == 'H')) ? true : false;

  // Simple clamping for safer inputs
  if (cfg.rpm < 100) cfg.rpm = 100; if (cfg.rpm > 6000) cfg.rpm = 6000;
  if (cfg.nTeeth == 0) cfg.nTeeth = 60;
  if (cfg.pMiss == 0) cfg.pMiss = 1;
  if (cfg.nMiss == 0) cfg.nMiss = 1;

  s_on_custom(&cfg);

  // Return to main and show a short status message
  show_main_page(NULL);
  if (lbl_status) lv_label_set_text(lbl_status, "Custom applied");
}

void ui_init(ui_on_pattern_cb on_pattern, ui_on_custom_cb on_custom) {
  s_on_pattern = on_pattern;
  s_on_custom  = on_custom;

  lv_init();
  register_display_stub();

  create_main_screen();
  lv_scr_load(screen_main);
}

void ui_task_handler() {
  // Basic tick/handler service; call this from loop()
  static uint32_t last = 0;
  uint32_t now = (uint32_t)millis();
  uint32_t diff = now - last;
  if (diff > 0) {
    lv_tick_inc(diff);
    last = now;
  }
  lv_timer_handler();
}

