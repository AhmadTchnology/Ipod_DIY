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
  if (S.btName[0] && !S.pairNext) {
    // a saved device triggers the BT stack's async connect allocations; give
    // them time to land before LVGL/UI consumes the remaining heap
    delay(2500);
    Serial.printf("[heap] after BT settle: %u\n", (unsigned)ESP.getFreeHeap());
  }
  Hal::lvglInit();
  Serial.printf("[heap] after lvgl: %u\n", (unsigned)ESP.getFreeHeap());
  UI::build();
  lv_mem_monitor_t m;
  lv_mem_monitor(&m);
  Serial.printf("[lvgl] pool total=%u free=%u maxused=%u frag=%u%%\n",
                (unsigned)m.total_size, (unsigned)m.free_size,
                (unsigned)m.max_used, (unsigned)m.frag_pct);
  Hal::applyPower();
  Serial.printf("Free heap after boot: %u bytes\n", (unsigned)ESP.getFreeHeap());
}

void loop() {
  static uint32_t lastMs = 0;               // trivial init — no C++ guard machinery
  uint32_t now = millis();
  lv_tick_inc(now - lastMs);
  lastMs = now;

  uint32_t wait = lv_timer_handler();
  Player::update();
  Hal::tick();
  Cfg::tick();

  // first-minute heap probe: traces the Bluetooth connect window + the UI at
  // runtime; self-terminates to keep the console quiet in normal use
  static uint32_t lastHp = 0;
  if (now - lastHp > 5000 && now < 90000) {
    lastHp = now;
    lv_mem_monitor_t m;
    lv_mem_monitor(&m);
    Serial.printf("[heap t=%us] free=%u min=%u lvglfree=%u\n", now / 1000,
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                  (unsigned)m.free_size);
  }

  delay(Hal::isScreenOn() ? constrain(wait, 3, 15) : 40);
}
