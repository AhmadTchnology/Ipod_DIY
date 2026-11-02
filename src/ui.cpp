#include "ui.h"
#include <lvgl.h>
#include "config.h"
#include "settings.h"
#include "library.h"
#include "player.h"
#include "hal.h"
#include "theme.h"
#include "touch.h"
#include "receiver.h"

// ------------------------------------------------------------------ palette
// Colours come from the theme module (saved look + accent, see Settings).
// C_BG/C_TEXT swap automatically when the Light look is chosen.
#define C_BG    Th_bg
#define C_SURF  Th_surf
#define C_SURF2 Th_sur2
#define C_TEXT  Th_text
#define C_DIM   Th_dim
#define C_ACC   Th_acc
// fixed companions: Bluetooth-connect blue and the album-art gradient end
#define C_BLUE  lv_color_hex(0x0A84FF)
#define C_PURP  lv_color_hex(0x5E5CE6)

#define ROW_H  46
#define POOL   8
#define HDR_H  40
#define TAB_H  46

// ------------------------------------------------------------------ state
struct Row { lv_obj_t *obj, *title, *sub, *icon; int idx; };

static bool uiActive = true;
static lv_obj_t *tv, *tabLib, *tabNow, *tabSet;
// library
static lv_obj_t *libCont, *libSpacer, *libCount, *libEmpty;
static Row rows[POOL];
// now playing
static lv_obj_t *npArt, *npTitle, *npArtist, *npSeek, *npEl, *npRem, *npBt, *npCnt;
static lv_obj_t *npPlay, *npPlayLbl, *npShuf, *npShufLbl, *npRep, *npRepLbl, *npVol;
// settings
static lv_obj_t *setBtLbl, *setInfo, *lblBright, *lblLimit;
// picker overlay
static lv_obj_t *pickPanel = nullptr, *pickList = nullptr;
static int pickShown = -1;

static lv_style_t stBtn, stBtnPress, stRow, stRowPress;
static lv_style_transition_dsc_t trDsc;
static const lv_style_prop_t trProps[] = {
  LV_STYLE_TRANSFORM_WIDTH, LV_STYLE_TRANSFORM_HEIGHT, LV_STYLE_BG_COLOR, LV_STYLE_BG_OPA, LV_STYLE_PROP_INV
};

static lv_anim_enable_t animFlag() { return S.animations ? LV_ANIM_ON : LV_ANIM_OFF; }

// ------------------------------------------------------------------ helpers
static void initStyles() {
  lv_style_transition_dsc_init(&trDsc, trProps, lv_anim_path_ease_out, 140, 0, NULL);
  lv_style_init(&stBtn);
  lv_style_set_transition(&stBtn, &trDsc);
  lv_style_init(&stBtnPress);
  lv_style_set_transform_width(&stBtnPress, -5);
  lv_style_set_transform_height(&stBtnPress, -5);
  lv_style_init(&stRow);
  lv_style_set_transition(&stRow, &trDsc);
  lv_style_set_border_side(&stRow, LV_BORDER_SIDE_BOTTOM);
  lv_style_set_border_width(&stRow, 1);
  lv_style_set_border_color(&stRow, C_SURF2);
  lv_style_init(&stRowPress);
  lv_style_set_bg_color(&stRowPress, C_SURF2);
  lv_style_set_bg_opa(&stRowPress, LV_OPA_COVER);
}

static lv_obj_t* mkLabel(lv_obj_t* p, const char* t, const lv_font_t* f, lv_color_t c) {
  lv_obj_t* l = lv_label_create(p);
  lv_label_set_text(l, t);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  return l;
}

static lv_obj_t* mkBtn(lv_obj_t* p, const char* sym, const lv_font_t* f, int w, int h,
                       lv_color_t fg, lv_event_cb_t cb) {
  lv_obj_t* b = lv_btn_create(p);
  lv_obj_remove_style_all(b);
  lv_obj_set_size(b, w, h);
  lv_obj_add_style(b, &stBtn, 0);
  lv_obj_add_style(b, &stBtnPress, LV_STATE_PRESSED);
  lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
  lv_obj_t* l = mkLabel(b, sym, f, fg);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
  return b;
}

static void fmtTime(char* o, size_t n, uint32_t s) {
  snprintf(o, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

// "pop in" = fade + small slide, used when the track changes
static void popExec(void* o, int32_t v) {
  lv_obj_set_style_opa((lv_obj_t*)o, (lv_opa_t)v, 0);
  lv_obj_set_style_translate_y((lv_obj_t*)o, (255 - v) / 20, 0);
}
static void popIn(lv_obj_t* o) {
  if (!S.animations) { popExec(o, 255); return; }
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, o);
  lv_anim_set_exec_cb(&a, popExec);
  lv_anim_set_values(&a, 0, 255);
  lv_anim_set_time(&a, 320);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);
}

// album-art "breathing" while music plays
static void breatheExec(void* o, int32_t v) {
  lv_obj_set_style_transform_width((lv_obj_t*)o, v, 0);
  lv_obj_set_style_transform_height((lv_obj_t*)o, v, 0);
}
static void setBreathing(bool on) {
  lv_anim_del(npArt, breatheExec);
  if (on && S.animations && uiActive) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, npArt);
    lv_anim_set_exec_cb(&a, breatheExec);
    lv_anim_set_values(&a, 0, 6);
    lv_anim_set_time(&a, 1100);
    lv_anim_set_playback_time(&a, 1100);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
  } else {
    breatheExec(npArt, 0);
  }
}

static void applyAnimSetting() {
  lv_obj_set_style_anim_time(lv_tabview_get_content(tv), S.animations ? 280 : 0, 0);
  setBreathing(Player::active());
}

// ------------------------------------------------------------------ library page (virtualised list)
static void fillRow(Row& r, int idx) {
  r.idx = idx;
  char t[64], a[48];
  Library::displayName(idx, t, sizeof(t), a, sizeof(a));
  lv_label_set_text(r.title, t);
  lv_label_set_text(r.sub, a[0] ? a : "Unknown artist");
  bool cur = (idx == Player::current());
  lv_obj_set_style_text_color(r.title, cur ? C_ACC : C_TEXT, 0);
  lv_obj_set_style_opa(r.icon, cur ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

static void layoutRows() {
  int total = Library::count();
  int first = lv_obj_get_scroll_y(libCont) / ROW_H;
  if (first < 0) first = 0;
  for (int k = 0; k < POOL; k++) {
    int idx = first + k;
    Row& r = rows[idx % POOL];
    if (idx >= total) { lv_obj_add_flag(r.obj, LV_OBJ_FLAG_HIDDEN); continue; }
    lv_obj_clear_flag(r.obj, LV_OBJ_FLAG_HIDDEN);
    if (r.idx != idx) fillRow(r, idx);
    lv_obj_set_y(r.obj, idx * ROW_H);
  }
}

static void forceRows() {
  for (int i = 0; i < POOL; i++) rows[i].idx = -1;
  layoutRows();
}

static void cbLibScroll(lv_event_t*) { layoutRows(); }

static void cbRow(lv_event_t* e) {
  int slot = (int)(intptr_t)lv_event_get_user_data(e);
  int idx = rows[slot].idx;
  if (idx < 0) return;
  if (!(idx == Player::current() && Player::loaded())) Player::play(idx);
  lv_tabview_set_act(tv, 1, animFlag());
}

void UI::refreshLibrary() {
  int n = Library::count();
  lv_obj_set_size(libSpacer, 1, (n > 0 ? n : 1) * ROW_H);
  // an empty spacer must not be scrollable: zero-height momentum scroll
  // on a nearly empty container has crashed LVGL under memory pressure
  if (n > 0) lv_obj_add_flag(libCont, LV_OBJ_FLAG_SCROLLABLE);
  else lv_obj_clear_flag(libCont, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_scroll_to_y(libCont, 0, LV_ANIM_OFF);
  lv_label_set_text_fmt(libCount, "%d songs", n);
  if (n == 0) {
    lv_label_set_text(libEmpty, Library::sdOk()
      ? "No songs found\n\nCopy .mp3 files into\n/music on the SD card"
      : "No SD card detected\n\nInsert a FAT32 card\nwith .mp3 files");
    lv_obj_clear_flag(libEmpty, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(libEmpty, LV_OBJ_FLAG_HIDDEN);
  }
  forceRows();
}

static void buildLibrary() {
  lv_obj_clear_flag(tabLib, LV_OBJ_FLAG_SCROLLABLE);
  mkLabel(tabLib, "Library", &lv_font_montserrat_20, C_TEXT);
  lv_obj_set_pos(lv_obj_get_child(tabLib, 0), 14, 8);
  libCount = mkLabel(tabLib, "", &lv_font_montserrat_12, C_DIM);
  lv_obj_align(libCount, LV_ALIGN_TOP_RIGHT, -14, 13);

  libCont = lv_obj_create(tabLib);
  lv_obj_remove_style_all(libCont);
  lv_obj_set_pos(libCont, 0, HDR_H);
  lv_obj_set_size(libCont, SCR_W, SCR_H - TAB_H - HDR_H);
  lv_obj_set_scroll_dir(libCont, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(libCont, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_add_flag(libCont, LV_OBJ_FLAG_SCROLL_MOMENTUM);

  libSpacer = lv_obj_create(libCont);
  lv_obj_remove_style_all(libSpacer);
  lv_obj_clear_flag(libSpacer, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_set_pos(libSpacer, 0, 0);

  for (int i = 0; i < POOL; i++) {
    Row& r = rows[i];
    r.idx = -1;
    r.obj = lv_obj_create(libCont);
    lv_obj_remove_style_all(r.obj);
    lv_obj_set_size(r.obj, SCR_W, ROW_H);
    lv_obj_add_style(r.obj, &stRow, 0);
    lv_obj_add_style(r.obj, &stRowPress, LV_STATE_PRESSED);
    lv_obj_clear_flag(r.obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(r.obj, LV_OBJ_FLAG_HIDDEN);
    r.title = mkLabel(r.obj, "", &lv_font_montserrat_14, C_TEXT);
    lv_obj_set_width(r.title, 185);
    lv_label_set_long_mode(r.title, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(r.title, 14, 5);
    r.sub = mkLabel(r.obj, "", &lv_font_montserrat_12, C_DIM);
    lv_obj_set_width(r.sub, 185);
    lv_label_set_long_mode(r.sub, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(r.sub, 14, 26);
    r.icon = mkLabel(r.obj, LV_SYMBOL_VOLUME_MID, &lv_font_montserrat_14, C_ACC);
    lv_obj_align(r.icon, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_obj_set_style_opa(r.icon, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(r.obj, cbRow, LV_EVENT_CLICKED, (void*)(intptr_t)i);
  }
  lv_obj_add_event_cb(libCont, cbLibScroll, LV_EVENT_SCROLL, NULL);

  libEmpty = mkLabel(tabLib, "", &lv_font_montserrat_14, C_DIM);
  lv_obj_set_style_text_align(libEmpty, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(libEmpty, LV_ALIGN_CENTER, 0, 10);
  UI::refreshLibrary();
}

// ------------------------------------------------------------------ now playing page
static void cbPlay(lv_event_t*) { Player::toggle(); }
static void cbNext(lv_event_t*) { Player::next(); }
static void cbPrev(lv_event_t*) { Player::prev(); }
static void cbShuf(lv_event_t*) { Player::setShuffle(!S.shuffle); }
static void cbRep(lv_event_t*)  { Player::setRepeat((S.repeat + 1) % 3); }
static void cbArt(lv_event_t*)  { Player::toggle(); }

static void cbSeek(lv_event_t* e) {
  lv_obj_t* s = lv_event_get_target(e);
  if (Player::loaded()) Player::seek(lv_slider_get_value(s) / 1000.0f);
}

static void cbVol(lv_event_t* e) {
  lv_obj_t* s = lv_event_get_target(e);
  Player::setVolume((uint8_t)lv_slider_get_value(s));
  if (lv_slider_get_value(s) > S.volume) lv_slider_set_value(s, S.volume, LV_ANIM_OFF);  // volume limit
}

static void styleSlider(lv_obj_t* s, int h) {
  lv_obj_set_height(s, h);
  lv_obj_set_style_bg_color(s, C_SURF2, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s, C_TEXT, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s, C_TEXT, LV_PART_KNOB);
  lv_obj_set_style_pad_all(s, 3, LV_PART_KNOB);
  lv_obj_set_ext_click_area(s, 14);
}

static void buildNowPlaying() {
  lv_obj_clear_flag(tabNow, LV_OBJ_FLAG_SCROLLABLE);

  npBt = mkLabel(tabNow, "", &lv_font_montserrat_12, C_DIM);
  lv_obj_set_pos(npBt, 12, 2);
  npCnt = mkLabel(tabNow, "", &lv_font_montserrat_12, C_DIM);
  lv_obj_align(npCnt, LV_ALIGN_TOP_RIGHT, -12, 2);

  npArt = lv_obj_create(tabNow);
  lv_obj_remove_style_all(npArt);
  lv_obj_set_size(npArt, 112, 112);
  lv_obj_align(npArt, LV_ALIGN_TOP_MID, 0, 20);
  lv_obj_set_style_radius(npArt, 24, 0);
  lv_obj_set_style_bg_opa(npArt, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(npArt, C_ACC, 0);
  lv_obj_set_style_bg_grad_color(npArt, C_PURP, 0);
  lv_obj_set_style_bg_grad_dir(npArt, LV_GRAD_DIR_VER, 0);
  lv_obj_clear_flag(npArt, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(npArt, cbArt, LV_EVENT_CLICKED, NULL);
  lv_obj_t* ic = mkLabel(npArt, LV_SYMBOL_AUDIO, &lv_font_montserrat_40, C_TEXT);
  lv_obj_set_style_text_opa(ic, LV_OPA_80, 0);
  lv_obj_center(ic);

  npTitle = mkLabel(tabNow, "Not playing", &lv_font_montserrat_16, C_TEXT);
  lv_obj_set_width(npTitle, 216);
  lv_label_set_long_mode(npTitle, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_style_text_align(npTitle, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(npTitle, LV_ALIGN_TOP_MID, 0, 140);

  npArtist = mkLabel(tabNow, "Pick a song in Library", &lv_font_montserrat_12, C_DIM);
  lv_obj_set_width(npArtist, 216);
  lv_label_set_long_mode(npArtist, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(npArtist, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(npArtist, LV_ALIGN_TOP_MID, 0, 162);

  npSeek = lv_slider_create(tabNow);
  lv_slider_set_range(npSeek, 0, 1000);
  lv_obj_set_width(npSeek, 208);
  styleSlider(npSeek, 5);
  lv_obj_align(npSeek, LV_ALIGN_TOP_MID, 0, 184);
  lv_obj_add_event_cb(npSeek, cbSeek, LV_EVENT_RELEASED, NULL);

  npEl = mkLabel(tabNow, "0:00", &lv_font_montserrat_12, C_DIM);
  lv_obj_set_pos(npEl, 16, 194);
  npRem = mkLabel(tabNow, "--:--", &lv_font_montserrat_12, C_DIM);
  lv_obj_align(npRem, LV_ALIGN_TOP_RIGHT, -16, 194);

  npPlay = mkBtn(tabNow, LV_SYMBOL_PLAY, &lv_font_montserrat_20, 46, 46, C_BG, cbPlay);
  lv_obj_set_style_bg_opa(npPlay, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(npPlay, C_TEXT, 0);
  lv_obj_align(npPlay, LV_ALIGN_TOP_MID, 0, 210);
  npPlayLbl = lv_obj_get_child(npPlay, 0);

  lv_obj_t* pv = mkBtn(tabNow, LV_SYMBOL_PREV, &lv_font_montserrat_20, 40, 40, C_TEXT, cbPrev);
  lv_obj_align(pv, LV_ALIGN_TOP_MID, -62, 213);
  lv_obj_t* nx = mkBtn(tabNow, LV_SYMBOL_NEXT, &lv_font_montserrat_20, 40, 40, C_TEXT, cbNext);
  lv_obj_align(nx, LV_ALIGN_TOP_MID, 62, 213);

  npShuf = mkBtn(tabNow, LV_SYMBOL_SHUFFLE, &lv_font_montserrat_16, 34, 34, C_DIM, cbShuf);
  lv_obj_align(npShuf, LV_ALIGN_TOP_MID, -102, 216);
  npShufLbl = lv_obj_get_child(npShuf, 0);
  npRep = mkBtn(tabNow, LV_SYMBOL_LOOP, &lv_font_montserrat_16, 34, 34, C_DIM, cbRep);
  lv_obj_align(npRep, LV_ALIGN_TOP_MID, 102, 216);
  npRepLbl = lv_obj_get_child(npRep, 0);

  mkLabel(tabNow, LV_SYMBOL_VOLUME_MID, &lv_font_montserrat_14, C_DIM);
  lv_obj_set_pos(lv_obj_get_child(tabNow, lv_obj_get_child_cnt(tabNow) - 1), 14, 257);
  npVol = lv_slider_create(tabNow);
  lv_slider_set_range(npVol, 0, 100);
  lv_slider_set_value(npVol, S.volume, LV_ANIM_OFF);
  lv_obj_set_width(npVol, 170);
  styleSlider(npVol, 5);
  lv_obj_set_pos(npVol, 44, 262);
  lv_obj_add_event_cb(npVol, cbVol, LV_EVENT_VALUE_CHANGED, NULL);
}

static void onTrackChanged() {
  char t[64], a[48];
  Player::meta(t, sizeof(t), a, sizeof(a));
  int cur = Player::current();
  if (cur < 0) {
    lv_label_set_text(npTitle, "Not playing");
    lv_label_set_text(npArtist, "Pick a song in Library");
    lv_label_set_text(npCnt, "");
  } else {
    lv_label_set_text(npTitle, t[0] ? t : "Unknown");
    lv_label_set_text(npArtist, a[0] ? a : "Unknown artist");
    lv_label_set_text_fmt(npCnt, "%d / %d", cur + 1, Library::count());
  }
  popIn(npTitle);
  popIn(npArtist);
  popIn(npArt);
  forceRows();
}

// ------------------------------------------------------------------ settings page
static lv_obj_t* setCard(const char* title) {
  lv_obj_t* h = mkLabel(tabSet, title, &lv_font_montserrat_12, C_DIM);
  lv_obj_set_style_pad_left(h, 4, 0);
  lv_obj_t* c = lv_obj_create(tabSet);
  lv_obj_remove_style_all(c);
  lv_obj_set_width(c, LV_PCT(100));
  lv_obj_set_height(c, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(c, C_SURF, 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(c, 12, 0);
  lv_obj_set_style_pad_all(c, 10, 0);
  lv_obj_set_style_pad_row(c, 10, 0);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  lv_obj_clear_flag(c, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  return c;
}

static lv_obj_t* setRow(lv_obj_t* card, const char* text, int ctrlW = 0, lv_obj_t** labelOut = nullptr) {
  lv_obj_t* r = lv_obj_create(card);
  lv_obj_remove_style_all(r);
  lv_obj_set_width(r, LV_PCT(100));
  lv_obj_set_height(r, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(r, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_t* l = mkLabel(r, text, &lv_font_montserrat_14, C_TEXT);
  if (ctrlW > 0) {
    // 240px screen - 2x10 tab pad - 2x10 card pad = 200px row content width.
    // Fixed label width + wrap makes label/control overlap impossible.
    lv_obj_set_width(l, 200 - ctrlW - 6);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  }
  if (labelOut) *labelOut = l;
  return r;
}

static lv_obj_t* addSwitch(lv_obj_t* card, const char* text, bool on, lv_event_cb_t cb) {
  lv_obj_t* r = setRow(card, text, 42);
  lv_obj_t* sw = lv_switch_create(r);
  lv_obj_set_size(sw, 42, 24);
  if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
  lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
  return sw;
}

static lv_obj_t* addDropdown(lv_obj_t* card, const char* text, const char* opts, int sel, lv_event_cb_t cb) {
  lv_obj_t* r = setRow(card, text, 92);
  lv_obj_t* dd = lv_dropdown_create(r);
  lv_dropdown_set_options(dd, opts);
  lv_dropdown_set_selected(dd, sel);
  lv_obj_set_width(dd, 92);
  lv_obj_set_style_pad_all(dd, 6, 0);
  lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, NULL);
  return dd;
}

static lv_obj_t* addSlider(lv_obj_t* card, const char* text, int mn, int mx, int val,
                           lv_event_cb_t cb, lv_obj_t** valLabel) {
  lv_obj_t* r = setRow(card, text, 100);
  lv_obj_t* box = lv_obj_create(r);
  lv_obj_remove_style_all(box);
  lv_obj_set_size(box, 100, 24);
  lv_obj_clear_flag(box, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_t* s = lv_slider_create(box);
  lv_slider_set_range(s, mn, mx);
  lv_slider_set_value(s, val, LV_ANIM_OFF);
  lv_obj_set_width(s, 60);
  styleSlider(s, 5);
  lv_obj_set_style_bg_color(s, C_ACC, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s, C_ACC, LV_PART_KNOB);
  lv_obj_align(s, LV_ALIGN_LEFT_MID, 4, 0);
  lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, NULL);
  *valLabel = mkLabel(box, "", &lv_font_montserrat_12, C_DIM);
  lv_obj_align(*valLabel, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_label_set_text_fmt(*valLabel, "%d%%", val);
  return s;
}

static lv_obj_t* addButton(lv_obj_t* card, const char* text, lv_event_cb_t cb) {
  lv_obj_t* b = lv_btn_create(card);
  lv_obj_remove_style_all(b);
  lv_obj_set_width(b, LV_PCT(100));
  lv_obj_set_height(b, 34);
  lv_obj_set_style_bg_color(b, C_SURF2, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(b, 10, 0);
  lv_obj_add_style(b, &stBtn, 0);
  lv_obj_add_style(b, &stBtnPress, LV_STATE_PRESSED);
  lv_obj_t* l = mkLabel(b, text, &lv_font_montserrat_14, C_TEXT);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
  return b;
}

static int ddSel(lv_event_t* e) { return lv_dropdown_get_selected(lv_event_get_target(e)); }
static bool swOn(lv_event_t* e) { return lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED); }

static void cbBright(lv_event_t* e) {
  int v = lv_slider_get_value(lv_event_get_target(e));
  S.brightness = v;
  lv_label_set_text_fmt(lblBright, "%d%%", v);
  Hal::applyBrightness();
  Cfg::saveSoon();
}
static void cbAuto(lv_event_t* e)    { S.autoBright = swOn(e); Hal::applyBrightness(); Cfg::saveSoon(); }
static void cbTimeout(lv_event_t* e) { S.timeoutIdx = ddSel(e); Cfg::saveSoon(); }
static void cbScrOff(lv_event_t*)    { Hal::screenOff(); }
static void cbCpu(lv_event_t* e)     { S.cpuIdx = ddSel(e); Hal::applyPower(); Cfg::saveSoon(); }
static void cbLed(lv_event_t* e)     { S.statusLed = swOn(e); Hal::applyPower(); Cfg::saveSoon(); }
static void cbPowerOff(lv_event_t* e){ S.powerOffIdx = ddSel(e); Cfg::saveSoon(); }
static void cbAnim(lv_event_t* e)    { S.animations = swOn(e); applyAnimSetting(); Cfg::saveSoon(); }
static void cbShufSw(lv_event_t* e)  { Player::setShuffle(swOn(e)); }
static void cbRepDd(lv_event_t* e)   { Player::setRepeat(ddSel(e)); }
static void cbLimit(lv_event_t* e) {
  int v = lv_slider_get_value(lv_event_get_target(e));
  S.volLimit = v;
  lv_label_set_text_fmt(lblLimit, "%d%%", v);
  Player::setVolume(S.volume);                       // re-clamp
  lv_slider_set_value(npVol, S.volume, LV_ANIM_OFF);
}
static void cbSleep(lv_event_t*) { Hal::deepSleep(); }

static void cbRescan(lv_event_t*) {
  Player::stop();
  delay(120);
  Library::rescan();
  UI::refreshLibrary();
  onTrackChanged();
}

// WiFi receiver switch: confirm via dialog, then restart into receiver mode
static void openDlg(int kind, const char* title, const char* txt);   // defined below
static void cbWifi(lv_event_t* e) {
  bool want = swOn(e);
  openDlg(want ? 3 : 4, "WiFi receiver",
          want
          ? "Restart into receiver mode?\n\nA hotspot opens: connect your phone to set your home WiFi and Telegram bot token. Songs sent to the bot land on the SD card."
          : "Turn off the WiFi receiver\nand restart the player?");
}

static void restartCb(lv_timer_t*) { ESP.restart(); }

// theme changes (kinds 5 = accent, 6 = look): NVS write + restart to apply,
// because the whole palette is baked in at boot when the UI is built
static int pendAcc = 0, pendLook = 0;
static void cbThemeSwatch(lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0 || idx >= 8 || idx == Th::accent()) return;
  pendAcc = idx;
  openDlg(5, "Accent colour", "Wipe your pod in this colour?\nThe player restarts to apply it.");
}
static void cbLookChip(lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0 || idx >= 4 || idx == Th::look()) return;
  pendLook = idx;
  openDlg(6, "Look", "Restart to switch the look?");
}

// confirm-then-restart dialogs
static const char* kBtns[]    = {"Restart", "Cancel", ""};
static const char* kBtnsOn[]  = {"Turn on", "Cancel", ""};
static const char* kBtnsOff[] = {"Turn off", "Cancel", ""};
static lv_obj_t* swWifi = nullptr;        // Settings wifi switch (set in buildSettings)
static int dlgKind = 0;   // 1 = pair, 2 = recalibrate, 3/4 = wifi, 5 = accent, 6 = look
static void cbDlg(lv_event_t* e) {
  lv_obj_t* mb = lv_event_get_current_target(e);
  uint16_t id = lv_msgbox_get_active_btn(mb);
  lv_msgbox_close(mb);
  if (id != 0) {
    if ((dlgKind == 3 || dlgKind == 4) && swWifi) {   // cancelled: reflect reality
      if (Receiver::enabled()) lv_obj_add_state(swWifi, LV_STATE_CHECKED);
      else lv_obj_clear_state(swWifi, LV_STATE_CHECKED);
    }
    return;
  }
  if (dlgKind == 1) { S.pairNext = 1; Cfg::save(); }
  if (dlgKind == 2) { Touch::forgetCal(); Cfg::save(); }
  if (dlgKind == 3 || dlgKind == 4) {
    Receiver::setEnabled(dlgKind == 3);
    ESP.restart();
  }
  if (dlgKind == 5) Th::setAccent((uint8_t)pendAcc);
  if (dlgKind == 6) Th::setLook((uint8_t)pendLook);
  ESP.restart();
}
static void openDlg(int kind, const char* title, const char* txt) {
  dlgKind = kind;
  const char** btns = kind == 3 ? kBtnsOn : kind == 4 ? kBtnsOff : kBtns;
  lv_obj_t* mb = lv_msgbox_create(NULL, title, txt, btns, false);
  lv_obj_set_width(mb, 210);
  lv_obj_add_event_cb(mb, cbDlg, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_center(mb);
}

static void showPicker();
static void cbPair(lv_event_t*) {
  if (Player::scanMode()) showPicker();
  else openDlg(1, "Pair headphones", "The player restarts to search for Bluetooth headphones. Put them in pairing mode.");
}
static void cbRecal(lv_event_t*) {
  openDlg(2, "Touch calibration", "The player restarts and asks you to tap three targets.");
}

static void buildSettings() {
  lv_obj_set_scroll_dir(tabSet, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(tabSet, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_style_pad_all(tabSet, 10, 0);
  lv_obj_set_style_pad_row(tabSet, 6, 0);
  lv_obj_set_flex_flow(tabSet, LV_FLEX_FLOW_COLUMN);

  lv_obj_t* hdr = mkLabel(tabSet, "Settings", &lv_font_montserrat_20, C_TEXT);
  lv_obj_set_style_pad_left(hdr, 4, 0);

  lv_obj_t* c = setCard("THEME");
  setRow(c, "Look");                                // label row, chips below
  lv_obj_t* lr = lv_obj_create(c);
  lv_obj_remove_style_all(lr);
  lv_obj_set_width(lr, LV_PCT(100));
  lv_obj_set_height(lr, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(lr, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(lr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(lr, 4, 0);
  lv_obj_set_style_pad_row(lr, 4, 0);
  lv_obj_clear_flag(lr, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  for (uint8_t i = 0; i < 4; i++) {
    lv_obj_t* b = lv_btn_create(lr);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 97, 26);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_bg_color(b, C_SURF2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    if (i == Th::look()) {
      lv_obj_set_style_border_width(b, 2, 0);
      lv_obj_set_style_border_color(b, C_TEXT, 0);
      lv_obj_set_style_border_opa(b, LV_OPA_COVER, 0);
    }
    lv_obj_t* lb = mkLabel(b, Th::LOOK_NAME[i], &lv_font_montserrat_12, C_TEXT);
    lv_obj_center(lb);
    lv_obj_add_event_cb(b, cbLookChip, LV_EVENT_CLICKED, (void*)(intptr_t)i);
  }
  setRow(c, "Accent colour");                       // label row, swatches on their own row
  lv_obj_t* swr = lv_obj_create(c);
  lv_obj_remove_style_all(swr);
  lv_obj_set_width(swr, LV_PCT(100));
  lv_obj_set_height(swr, 21);
  lv_obj_set_flex_flow(swr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(swr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(swr, 4, 0);
  lv_obj_clear_flag(swr, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  for (uint8_t i = 0; i < 8; i++) {
    lv_obj_t* b = lv_btn_create(swr);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 19, 19);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(Th::accentHex(i)), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    if (i == Th::accent()) {
      lv_obj_set_style_border_width(b, 2, 0);
      lv_obj_set_style_border_color(b, C_TEXT, 0);
      lv_obj_set_style_border_opa(b, LV_OPA_COVER, 0);
    }
    lv_obj_add_event_cb(b, cbThemeSwatch, LV_EVENT_CLICKED, (void*)(intptr_t)i);
  }

  c = setCard("BLUETOOTH");
  lv_obj_t* r = setRow(c, "Headphones");
  setBtLbl = mkLabel(r, "", &lv_font_montserrat_12, C_DIM);
  lv_obj_set_width(setBtLbl, 110);
  lv_label_set_long_mode(setBtLbl, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(setBtLbl, LV_TEXT_ALIGN_RIGHT, 0);
  addButton(c, LV_SYMBOL_BLUETOOTH "  Pair new headphones", cbPair);

  c = setCard("WIFI RECEIVER");
  swWifi = addSwitch(c, "WiFi receiver", Receiver::enabled(), cbWifi);
  lv_obj_t* wi = mkLabel(c, "Hotspot portal for your phone: enter home WiFi + Telegram bot token. Songs the bot receives go to the SD card (/music/telegram).", &lv_font_montserrat_12, C_DIM);
  lv_obj_set_width(wi, LV_PCT(100));
  lv_label_set_long_mode(wi, LV_LABEL_LONG_WRAP);

  c = setCard("DISPLAY");
  addSlider(c, "Brightness", 5, 100, S.brightness, cbBright, &lblBright);
  addSwitch(c, "Auto brightness", S.autoBright, cbAuto);
  addDropdown(c, "Screen off after", "Never\n15 s\n30 s\n1 min\n2 min", S.timeoutIdx, cbTimeout);
  addButton(c, LV_SYMBOL_EYE_OPEN "  Turn screen off now", cbScrOff);

  c = setCard("POWER SAVING");
  addDropdown(c, "CPU speed", "80 MHz\n160 MHz\n240 MHz", S.cpuIdx, cbCpu);
  addSwitch(c, "Status LED", S.statusLed, cbLed);
  addSwitch(c, "Animations", S.animations, cbAnim);
  addDropdown(c, "Auto power-off", "Never\n5 min\n10 min\n30 min", S.powerOffIdx, cbPowerOff);
  addButton(c, LV_SYMBOL_POWER "  Sleep now (touch wakes)", cbSleep);

  c = setCard("PLAYBACK");
  addSwitch(c, "Shuffle", S.shuffle, cbShufSw);
  addDropdown(c, "Repeat", "Off\nAll\nOne", S.repeat, cbRepDd);
  addSlider(c, "Volume limit", 20, 100, S.volLimit, cbLimit, &lblLimit);

  c = setCard("SYSTEM");
  addButton(c, LV_SYMBOL_REFRESH "  Rescan music library", cbRescan);
  addButton(c, LV_SYMBOL_EDIT "  Recalibrate touch", cbRecal);
  setInfo = mkLabel(c, "", &lv_font_montserrat_12, C_DIM);
  lv_obj_set_width(setInfo, LV_PCT(100));
}

// ------------------------------------------------------------------ headphone picker overlay
static void cbPickCancel(lv_event_t*) {
  if (S.pairNext) { S.pairNext = 0; Cfg::save(); ESP.restart(); }
  lv_obj_del(pickPanel);
  pickPanel = nullptr; pickList = nullptr; pickShown = -1;
}

static void cbPick(lv_event_t* e) {
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  strlcpy(S.btName, Player::scanName(i), sizeof(S.btName));
  S.pairNext = 0;
  Cfg::save();
  lv_obj_t* l = mkLabel(pickPanel, "Connecting...", &lv_font_montserrat_16, C_TEXT);
  lv_obj_center(l);
  lv_obj_clean(pickList);
  lv_timer_create(restartCb, 600, NULL);
}

static void showPicker() {
  if (pickPanel) return;
  pickPanel = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(pickPanel);
  lv_obj_set_size(pickPanel, SCR_W, SCR_H);
  lv_obj_set_style_bg_color(pickPanel, C_BG, 0);
  lv_obj_set_style_bg_opa(pickPanel, LV_OPA_COVER, 0);
  lv_obj_clear_flag(pickPanel, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* t = mkLabel(pickPanel, "Pair headphones", &lv_font_montserrat_20, C_TEXT);
  lv_obj_set_pos(t, 14, 12);
  lv_obj_t* h = mkLabel(pickPanel, "Put your headphones in pairing mode. They show up below - tap yours.",
                        &lv_font_montserrat_12, C_DIM);
  lv_obj_set_width(h, 212);
  lv_obj_set_pos(h, 14, 44);
  pickList = lv_list_create(pickPanel);
  lv_obj_set_size(pickList, 212, 180);
  lv_obj_set_pos(pickList, 14, 84);
  lv_obj_set_style_bg_color(pickList, C_SURF, 0);
  lv_obj_set_style_border_width(pickList, 0, 0);
  lv_obj_t* b = addButton(pickPanel, "Cancel", cbPickCancel);
  lv_obj_set_width(b, 212);
  lv_obj_set_pos(b, 14, 276);
  pickShown = -1;
}

// ------------------------------------------------------------------ periodic refresh
static void uiTick(lv_timer_t*) {
  if (!uiActive) return;

  static uint32_t lastSerial = 0xFFFFFFFF;
  uint32_t ser = Player::serial();
  if (ser != lastSerial) { lastSerial = ser; onTrackChanged(); }

  static int lastAct = -1;
  bool act = Player::active();
  if (act != lastAct) {
    lastAct = act;
    lv_label_set_text(npPlayLbl, act ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    setBreathing(act);
  }

  static int lastShuf = -1, lastRep = -1;
  if (lastShuf != S.shuffle) {
    lastShuf = S.shuffle;
    lv_obj_set_style_text_color(npShufLbl, S.shuffle ? C_ACC : C_DIM, 0);
  }
  if (lastRep != S.repeat) {
    lastRep = S.repeat;
    lv_label_set_text(npRepLbl, S.repeat == 2 ? LV_SYMBOL_LOOP "1" : LV_SYMBOL_LOOP);
    lv_obj_set_style_text_color(npRepLbl, S.repeat ? C_ACC : C_DIM, 0);
  }

  if (!lv_slider_is_dragged(npSeek))
    lv_slider_set_value(npSeek, (int)(Player::fraction() * 1000), animFlag());

  static uint32_t lastEl = 0xFFFFFFFF, lastTot = 0xFFFFFFFF;
  uint32_t el = (uint32_t)Player::position(), tot = Player::duration();
  if (!Player::loaded()) { el = 0; tot = 0; }
  if (el != lastEl || tot != lastTot) {
    lastEl = el; lastTot = tot;
    char b[12];
    fmtTime(b, sizeof(b), el);
    lv_label_set_text(npEl, b);
    if (tot) { fmtTime(b, sizeof(b), tot > el ? tot - el : 0); char m[14]; snprintf(m, sizeof(m), "-%s", b); lv_label_set_text(npRem, m); }
    else lv_label_set_text(npRem, "--:--");
  }

  bool conn = Player::btConnected();
  if (Player::scanMode()) {
    lv_label_set_text(npBt, LV_SYMBOL_BLUETOOTH "  Pairing mode");
    lv_obj_set_style_text_color(npBt, C_ACC, 0);
  } else if (conn) {
    lv_label_set_text(npBt, LV_SYMBOL_BLUETOOTH "  Connected");
    lv_obj_set_style_text_color(npBt, C_BLUE, 0);
  } else {
    lv_label_set_text(npBt, LV_SYMBOL_BLUETOOTH "  Searching...");
    lv_obj_set_style_text_color(npBt, C_DIM, 0);
  }

  if (pickPanel && pickList && Player::scanCount() != pickShown) {
    pickShown = Player::scanCount();
    lv_obj_clean(pickList);
    for (int i = 0; i < pickShown; i++) {
      lv_obj_t* b = lv_list_add_btn(pickList, LV_SYMBOL_BLUETOOTH, Player::scanName(i));
      lv_obj_add_event_cb(b, cbPick, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }
  }

  if (lv_tabview_get_tab_act(tv) == 2) {
    static uint32_t lastInfo = 0;
    if (millis() - lastInfo > 1500) {
      lastInfo = millis();
      lv_label_set_text_fmt(setInfo, "%d tracks  |  heap %u KB  |  v%s",
                            Library::count(), (unsigned)(ESP.getFreeHeap() / 1024), FW_VERSION);
      lv_label_set_text(setBtLbl, S.btName[0]
        ? S.btName : (Player::scanMode() ? "pairing..." : "not paired"));
    }
  }
}

void UI::setActive(bool on) {
  uiActive = on;
  if (tv) setBreathing(on && Player::active());
}

void UI::build() {
  initStyles();
  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, C_BG, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  tv = lv_tabview_create(scr, LV_DIR_BOTTOM, TAB_H);
  lv_obj_set_size(tv, SCR_W, SCR_H);
  lv_obj_set_style_bg_color(tv, C_BG, 0);

  lv_obj_t* content = lv_tabview_get_content(tv);
  lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);       // pages change via the tab bar only
  lv_obj_set_style_bg_color(content, C_BG, 0);

  lv_obj_t* btns = lv_tabview_get_tab_btns(tv);
  lv_obj_set_style_bg_color(btns, C_SURF, 0);
  lv_obj_set_style_text_font(btns, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(btns, C_DIM, 0);
  lv_obj_set_style_text_color(btns, C_ACC, LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_bg_opa(btns, LV_OPA_TRANSP, LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(btns, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_bg_opa(btns, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_border_side(btns, LV_BORDER_SIDE_TOP, LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_border_width(btns, 3, LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_border_color(btns, C_ACC, LV_PART_ITEMS | LV_STATE_CHECKED);

  tabLib = lv_tabview_add_tab(tv, LV_SYMBOL_LIST);
  tabNow = lv_tabview_add_tab(tv, LV_SYMBOL_AUDIO);
  tabSet = lv_tabview_add_tab(tv, LV_SYMBOL_SETTINGS);
  lv_obj_t* tabs[3] = {tabLib, tabNow, tabSet};
  for (int i = 0; i < 3; i++) {
    lv_obj_set_style_pad_all(tabs[i], 0, 0);
    lv_obj_set_style_border_width(tabs[i], 0, 0);
    lv_obj_set_style_bg_opa(tabs[i], LV_OPA_TRANSP, 0);
  }
  lv_obj_set_style_pad_all(tabSet, 10, 0);

  buildLibrary();
  buildNowPlaying();
  buildSettings();
  applyAnimSetting();
  onTrackChanged();

  lv_timer_create(uiTick, 250, NULL);

  if (Player::scanMode()) {
    lv_tabview_set_act(tv, 2, LV_ANIM_OFF);
    showPicker();
  }

  // gentle intro: whole UI fades in
  lv_obj_set_style_opa(tv, LV_OPA_TRANSP, 0);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, tv);
  lv_anim_set_exec_cb(&a, [](void* o, int32_t v) { lv_obj_set_style_opa((lv_obj_t*)o, (lv_opa_t)v, 0); });
  lv_anim_set_values(&a, 0, 255);
  lv_anim_set_time(&a, S.animations ? 450 : 1);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);
}
