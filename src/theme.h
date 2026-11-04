#pragma once
#include <Arduino.h>

// Theme module: preset looks (dark variants + light) and accent colours.
// Pure RGB565 palette — no UI toolkit involved, so a change applies instantly
// (no restart). Look + accent are stored as their own NVS keys ("mypod"/"look",
// "mypod"/"accent") so the Settings blob never needs a version bump.

namespace Th {
  extern uint16_t bg, surf, surf2, txt, dim, acc;   // current palette (RGB565)

  extern const char* const LOOK_NAME[4];
  extern const char* const ACC_NAME[8];

  void     load();                      // read NVS, fill the palette
  uint8_t  look();
  uint8_t  accent();
  uint32_t accentHex(uint8_t idx);
  uint16_t rgb(uint32_t hex);           // 0xRRGGBB -> RGB565
  uint16_t blend(uint16_t a, uint16_t b, uint8_t t);   // t=0 -> a, t=255 -> b

  void setLook(uint8_t idx);            // persist + apply immediately
  void setAccent(uint8_t idx);
}
