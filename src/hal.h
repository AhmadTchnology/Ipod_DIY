#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

namespace Hal {
  extern TFT_eSPI tft;
  void earlyInit();                    // pins, backlight PWM, TFT, splash
  void setBrightness(int pct);
  void fadeTo(int pct, uint16_t ms);
  void applyBrightness();
  void applyPower();                   // CPU frequency + LED state
  void tick();                         // screen timeout, auto-brightness, LED, auto power-off
  void screenOff();
  void screenOn();
  bool isScreenOn();
  void deepSleep();

  // Touch with wake-swallow: the touch that wakes a dark screen is never delivered.
  bool readTouch(int16_t& x, int16_t& y);

  // Software clock (no RTC on the CYD). Survives deep sleep, not a power cycle.
  bool clockValid();
  void clockGet(int& h, int& m);
  void clockSet(int h, int m);

  int  batteryPercent();               // -1 when no BAT_ADC_PIN is configured
}
