#include "theme.h"
#include <Preferences.h>

lv_color_t Th_bg, Th_surf, Th_sur2, Th_text, Th_dim, Th_acc;

namespace Th {

// Dark, Midnight (navy), Slate (warm dark grey), Light
struct Look { uint32_t bg, surf, surf2, txt, dim; bool dark; };
static const Look LOOKS[] = {
  {0x000000, 0x1C1C1E, 0x2C2C2E, 0xFFFFFF, 0x8E8E93, true},    // Dark   (original)
  {0x0A0E14, 0x161C26, 0x232C3A, 0xE6EDF3, 0x7D8590, true},   // Midnight
  {0x14161D, 0x1D2029, 0x282C38, 0xF3F4F8, 0x9AA0AE, true},   // Slate
  {0xF2F2F7, 0xFFFFFF, 0xE5E5EA, 0x111114, 0x6D6D72, false},  // Light
};

const char* const LOOK_NAME[4] = {"Dark", "Midnight", "Slate", "Light"};

static const uint32_t ACCENTS[8] = {
  0xFA2D48,  // Red    (original)
  0x0A84FF,  // Blue
  0x5E5CE6,  // Purple
  0x30D158,  // Green
  0xFF9F0A,  // Orange
  0xFF375F,  // Pink
  0x5AC8FA,  // Cyan
  0xFFD60A,  // Gold
};

const char* const ACC_NAME[8] = {"Red", "Blue", "Purple", "Green",
                                 "Orange", "Pink", "Cyan", "Gold"};

static uint8_t loadIdx(const char* key, uint8_t def, uint8_t count) {
  Preferences p;
  p.begin("mypod", true);
  uint8_t v = p.getUChar(key, def);
  p.end();
  if (v >= count) v = def;
  return v;
}

static void saveIdx(const char* key, uint8_t idx) {
  Preferences p;
  p.begin("mypod", false);
  p.putUChar(key, idx);
  p.end();
}

uint8_t look()   { return loadIdx("look", 0, 4); }
uint8_t accent() { return loadIdx("accent", 0, 8); }

uint32_t accentHex(uint8_t idx) {
  if (idx >= 8) idx = 0;
  return ACCENTS[idx];
}

void setLook(uint8_t idx)   { if (idx < 4) saveIdx("look", idx); }
void setAccent(uint8_t idx) { if (idx < 8) saveIdx("accent", idx); }

void apply() {
  const Look& L = LOOKS[look()];
  Th_bg   = lv_color_hex(L.bg);
  Th_surf = lv_color_hex(L.surf);
  Th_sur2 = lv_color_hex(L.surf2);
  Th_text = lv_color_hex(L.txt);
  Th_dim  = lv_color_hex(L.dim);
  Th_acc  = lv_color_hex(ACCENTS[accent()]);

  lv_disp_t* d = lv_disp_get_default();
  if (d) {
    // accent drives both primary + secondary so switches, dropdown lists,
    // msgboxes and the tab indicator all follow the user's colour
    lv_theme_t* th = lv_theme_default_init(d, Th_acc, Th_acc, L.dark,
                                           &lv_font_montserrat_14);
    lv_disp_set_theme(d, th);
  }
}

}  // namespace Th
