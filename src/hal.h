#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

namespace Hal {
  extern TFT_eSPI tft;
  void earlyInit();                    // pins, backlight PWM, TFT, splash
  void lvglInit();                     // LVGL display + touch drivers + theme
  void setBrightness(int pct);
  void fadeTo(int pct, uint16_t ms);
  void applyBrightness();
  void applyPower();                   // CPU frequency + LED state
  void tick();                         // screen timeout, auto-brightness, LED, auto power-off
  void screenOff();
  void screenOn();
  bool isScreenOn();
  void deepSleep();
}
