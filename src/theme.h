#pragma once
#include <Arduino.h>
#include <lvgl.h>

// Theme module: preset looks (dark variants + light) and accent colours.
// Stored as separate NVS keys ("mypod"/"look" + "mypod"/"accent") so the
// player's Settings blob never needs a version bump or migration.
//
// Hal::lvglInit() calls Th::apply() right after the display driver is
// registered and BEFORE any UI object exists, so every style created
// with the C_* macros in ui.cpp picks up the saved palette.

extern lv_color_t Th_bg, Th_surf, Th_sur2, Th_text, Th_dim, Th_acc;

namespace Th {
  extern const char* const LOOK_NAME[4];   // names for the dropdown
  extern const char* const ACC_NAME[8];

  uint8_t  look();                         // current look index 0..3
  uint8_t  accent();                       // current accent index 0..7
  uint32_t accentHex(uint8_t idx);         // raw colour for swatches

  void setLook(uint8_t idx);               // persist (caller restarts to apply)
  void setAccent(uint8_t idx);

  void apply();                            // fill palette + init lv theme
}
