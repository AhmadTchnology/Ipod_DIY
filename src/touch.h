#pragma once
#include <Arduino.h>
class TFT_eSPI;

namespace Touch {
  void begin();
  bool readRaw(uint16_t& x, uint16_t& y);   // true while pressed
  bool read(int16_t& sx, int16_t& sy);      // screen coordinates
  bool loadCal();
  void calibrate(TFT_eSPI& tft);            // 3-tap wizard, saves result
  void forgetCal();
}
