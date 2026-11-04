#include <Arduino.h>
#ifdef MP_BTBARE
// ------------------------------------------------------------------ DIAGNOSTIC
// Bare-boot A2DP repro: no TFT/Touch/LVGL/Cfg/SD/UI — nothing from MyPod runs
// except Serial, a delay and the BluetoothA2DPSource start sequence. Build with
// -D MP_BTBARE=1 (env:btbare) to use it; the full app boots without the flag.
// With -D MP_BTMIMIC=1 (env:btmimic) it additionally runs Player::prealloc()+begin()
// and pins a heap blob so free memory lands at the full app's play-time budget
// (~105 KB), to test whether only the heap SIZE is what breaks bluedroid.
#include "player.h"
void setup() {
  Serial.begin(115200);
  Serial.printf("[bare] heap start %u\n", (unsigned)ESP.getFreeHeap());
  // PSRAM probe: run with -D BOARD_HAS_PSRAM to attempt the controller init
  Serial.printf("[psram] found=%d size=%u free=%u\n", (int)psramFound(),
                (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());
  void* lump = ps_malloc(1000000);
  Serial.printf("[psram] 1MB alloc -> %p %s\n", lump, lump ? "OK" : "(nullptr)");
  if (lump) { memset(lump, 0x5A, 1000000); free(lump); Serial.println("[psram] write/read/free ok"); }
#ifdef MP_BTMIMIC
  Player::prealloc();
  Player::begin();
  Serial.printf("[mimic] after player init: free=%u largest=%u\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  size_t want = ESP.getFreeHeap();
  void* blob = nullptr;
  if (want > 121000) {
    blob = malloc(want - 106000);              // leave ~105.5 KB, like the app
    if (blob) memset(blob, 0xA5, want - 106000);
  }
  Serial.printf("[mimic] pinned=%p -> free=%u largest=%u\n", blob,
                (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
#endif
  Serial.println("[bare] starting Bluetooth in 8s (nothing else is running)...");
  delay(8000);
  Player::ensureBt();
  Serial.printf("[bare] heap after ensureBt %u\n", (unsigned)ESP.getFreeHeap());
}
void loop() {
  static uint32_t lastMs = 0;
  if (millis() - lastMs > 5000) {
    lastMs = millis();
    Serial.printf("[bare t=%us] free=%u min=%u\n", lastMs / 1000,
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
  }
  delay(50);
}
#else

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
  Player::prealloc();                       // pin helix buffers before anything else
  Player::begin();                          // PCM ring + decoder task (Bluetooth waits for play)
  Touch::begin();
  pinMode(PIN_BOOT, INPUT_PULLUP);
  if (!Touch::loadCal() || digitalRead(PIN_BOOT) == LOW) Touch::calibrate(Hal::tft);
  // the SD card and Bluetooth come up lazily on first play / library view:
  // the A2DP connect step needs ~105+ KB free and only exists there
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
    Serial.printf("[heap t=%us] free=%u min=%u lvglfree=%u btup=%d conn=%d\n", now / 1000,
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                  (unsigned)m.free_size,
                  (int)Player::btStarted(), (int)Player::btConnected());
  }

  delay(Hal::isScreenOn() ? constrain(wait, 3, 15) : 40);
}
#endif
