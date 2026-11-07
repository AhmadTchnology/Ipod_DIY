#include <Arduino.h>
#include "config.h"
#include "settings.h"
#include "theme.h"
#include "touch.h"
#include "hal.h"
#include "library.h"
#include "player.h"
#include "ui.h"
#include "usbsync.h"
#include <esp_timer.h>

void setup() {
  Serial.begin(115200);
  Serial.printf("[heap] start: %u\n", (unsigned)ESP.getFreeHeap());
  Cfg::load();
  Th::load();
  Hal::earlyInit();                         // backlight, TFT, splash
  if (UsbSync::enabled()) UsbSync::run();   // USB sync mode (PC tool), never returns
  Serial.printf("[heap] after early: %u\n", (unsigned)ESP.getFreeHeap());
  Touch::begin();
  pinMode(PIN_BOOT, INPUT_PULLUP);
  if (!Touch::loadCal() || digitalRead(PIN_BOOT) == LOW) Touch::calibrate(Hal::tft);
  Library::begin();                         // SD + index
  Serial.printf("[heap] after library: %u\n", (unsigned)ESP.getFreeHeap());
  // Claim the MP3 decoder's permanent buffers BEFORE Bluetooth: the BT stack
  // (BTDM, ~92 KB incl. connect burst) then comes up against a fatter heap and
  // the A2DP connect dip lands around ~20 KB free instead of the ~8 KB floor
  // that starved the SBC encoder and fopen at first play.
  Player::finishInit();                     // decoder buffers, allocated ONCE
  Serial.printf("[heap] after decoder pre-pin: %u\n", (unsigned)ESP.getFreeHeap());
  Player::begin();                          // ring + decoder task + Bluetooth
  if (S.btName[0] && !S.pairNext) {
    // a saved device triggers the BT stack's async connect allocations; give
    // them time to land
    delay(2500);
    Serial.printf("[heap] after BT settle: %u largest=%u\n",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT |
                                                             MALLOC_CAP_INTERNAL));
  }
  UI::begin();
  Hal::applyPower();
  Serial.printf("[heap] ready: free=%u maxblock=%u\n", (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMaxAllocHeap());
}

void loop() {
  uint32_t lw0 = (uint32_t)esp_timer_get_time();
  UI::update();
  Player::update();
  Hal::tick();
  Cfg::tick();
  uint32_t lw1 = (uint32_t)esp_timer_get_time();
  {                       // loop-task diagnostics on the audio-cut investigation
    static uint32_t maxUs = 0, over = 0, last = 0;
    uint32_t dt = lw1 - lw0;
    if (dt > maxUs) maxUs = dt;
    if (dt > 10000) over++;
    if (millis() - last > 5000 && millis() > 15000) {
      last = millis();
      Serial.printf("[loop] work=%luus max=%luus over10ms=%u\n",
                    (unsigned long)dt, (unsigned long)maxUs, (unsigned)over);
      maxUs = 0; over = 0;
    }
  }

  // heap probe for the first 90 s; self-terminates to keep the console quiet
  static uint32_t lastHp = 0;
  uint32_t now = millis();
  if (now - lastHp > 5000 && now < 90000) {
    lastHp = now;
    Serial.printf("[heap t=%us] free=%u min=%u maxblock=%u\n", now / 1000,
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                  (unsigned)ESP.getMaxAllocHeap());
  }

  delay(Hal::isScreenOn() ? UI::idleDelayMs() : 40);
}
