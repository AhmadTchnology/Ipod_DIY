#include "theme.h"
#include <Preferences.h>

namespace Th {

uint16_t bg, surf, surf2, txt, dim, acc;

struct Look { uint32_t bg, surf, surf2, txt, dim; };
static const Look LOOKS[] = {
  {0x000000, 0x1C1C1E, 0x2C2C2E, 0xFFFFFF, 0x8E8E93},   // Dark
  {0x0A0E14, 0x161C26, 0x232C3A, 0xE6EDF3, 0x7D8590},   // Midnight
  {0x14161D, 0x1D2029, 0x282C38, 0xF3F4F8, 0x9AA0AE},   // Slate
  {0xF2F2F7, 0xFFFFFF, 0xE5E5EA, 0x111114, 0x6D6D72},   // Light
};
const char* const LOOK_NAME[4] = {"Dark", "Midnight", "Slate", "Light"};

static const uint32_t ACCENTS[8] = {
  0xFA2D48, 0x0A84FF, 0x5E5CE6, 0x30D158, 0xFF9F0A, 0xFF375F, 0x5AC8FA, 0xFFD60A,
};
const char* const ACC_NAME[8] = {"Red", "Blue", "Purple", "Green",
                                 "Orange", "Pink", "Cyan", "Gold"};

static const uint8_t DEF_LOOK = 0;
static const uint8_t DEF_ACC  = 3;     // Green: matches the KLIK MP3 player design

static uint8_t curLook = DEF_LOOK, curAcc = DEF_ACC;

uint16_t rgb(uint32_t h) {
  return (uint16_t)(((h >> 8) & 0xF800) | ((h >> 5) & 0x07E0) | ((h >> 3) & 0x001F));
}

uint16_t blend(uint16_t a, uint16_t b, uint8_t t) {
  uint32_t ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
  uint32_t br = b >> 11, bg_ = (b >> 5) & 63, bb = b & 31;
  uint32_t r = (ar * (255 - t) + br * t) / 255;
  uint32_t g = (ag * (255 - t) + bg_ * t) / 255;
  uint32_t bl = (ab * (255 - t) + bb * t) / 255;
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

static void fill() {
  const Look& L = LOOKS[curLook];
  bg = rgb(L.bg); surf = rgb(L.surf); surf2 = rgb(L.surf2);
  txt = rgb(L.txt); dim = rgb(L.dim); acc = rgb(ACCENTS[curAcc]);
}

static uint8_t loadIdx(const char* key, uint8_t def, uint8_t count) {
  Preferences p;
  p.begin("mypod", true);
  uint8_t v = p.getUChar(key, def);
  p.end();
  return v >= count ? def : v;
}

static void saveIdx(const char* key, uint8_t idx) {
  Preferences p;
  p.begin("mypod", false);
  p.putUChar(key, idx);
  p.end();
}

void load() {
  curLook = loadIdx("look", DEF_LOOK, 4);
  curAcc  = loadIdx("accent", DEF_ACC, 8);
  fill();
}

uint8_t look()   { return curLook; }
uint8_t accent() { return curAcc; }
uint32_t accentHex(uint8_t idx) { return ACCENTS[idx >= 8 ? 0 : idx]; }

void setLook(uint8_t idx)   { if (idx < 4) { curLook = idx; saveIdx("look", idx); fill(); } }
void setAccent(uint8_t idx) { if (idx < 8) { curAcc = idx;  saveIdx("accent", idx); fill(); } }

}  // namespace Th
