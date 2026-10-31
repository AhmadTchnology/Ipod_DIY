#include <Arduino.h>
#include <lvgl.h>
#include "config.h"
#include "settings.h"
#include "touch.h"
#include "hal.h"
#include "library.h"
#include "player.h"
#include "ui.h"
#include "receiver.h"

void setup() {
  Serial.begin(115200);
  Serial.printf("[heap] start: %u\n", (unsigned)ESP.getFreeHeap());
  Cfg::load();
  Hal::earlyInit();                         // backlight, TFT, splash
  if (Receiver::enabled()) Receiver::run(); // WiFi receiver mode, never returns
  Serial.printf("[heap] after early: %u\n", (unsigned)ESP.getFreeHeap());
  Touch::begin();
  pinMode(PIN_BOOT, INPUT_PULLUP);
  if (!Touch::loadCal() || digitalRead(PIN_BOOT) == LOW) Touch::calibrate(Hal::tft);
  Library::begin();                         // SD + scan
  Serial.printf("[heap] after library: %u\n", (unsigned)ESP.getFreeHeap());
  Player::begin();                          // decoder task + Bluetooth (allocate before LVGL grows)
  Hal::lvglInit();
  Serial.printf("[heap] after lvgl: %u\n", (unsigned)ESP.getFreeHeap());
  UI::build();
  Hal::applyPower();
  Serial.printf("Free heap after boot: %u bytes\n", (unsigned)ESP.getFreeHeap());
}

void loop() {
  static uint32_t lastMs = millis();
  uint32_t now = millis();
  lv_tick_inc(now - lastMs);
  lastMs = now;

  uint32_t wait = lv_timer_handler();
  Player::update();
  Hal::tick();
  Cfg::tick();

  delay(Hal::isScreenOn() ? constrain(wait, 3, 15) : 40);
}
