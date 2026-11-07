// =====================================================================
//  USB sync - a separate boot mode, chosen in Settings.
//
//  Like the old receiver it never starts Bluetooth or the player, so the
//  whole heap is free for SD writes. The PC tool ("MyPod Sync") sends
//  songs and their 200x200 cover over the USB serial cable; see
//  usbsync_engine.h for the protocol.
//
//  Exit: the PC tool's "Exit" button, or hold BOOT for 3 s.
// =====================================================================
#include "usbsync.h"
#include "usbsync_engine.h"
#include "config.h"
#include "hal.h"
#include "settings.h"
#include <Preferences.h>
#include <esp_log.h>

namespace UsbSync {

static void flagWrite(uint8_t v) {
  Preferences p;
  p.begin("mypod", false);
  p.putUChar("usb", v);
  p.end();
}
bool enabled() {
  Preferences p;
  p.begin("mypod", true);
  uint8_t v = p.getUChar("usb", 0);
  p.end();
  return v != 0;
}
void setEnabled(bool on) { flagWrite(on ? 1 : 0); }

static void exitToPlayer() {
  flagWrite(0);
  Serial.flush();
  delay(150);
  ESP.restart();
}

static SPIClass sdSpi(VSPI);
static bool sdBegin() {
  sdSpi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  return SD.begin(SD_CS, sdSpi, 20000000);
}

struct SerialIO : IO {
  size_t readSome(uint8_t* b, size_t n) override {
    int a = Serial.available();
    if (a <= 0) return 0;
    if ((size_t)a < n) n = a;
    return Serial.readBytes(b, n);
  }
  void write(const uint8_t* b, size_t n) override { Serial.write(b, n); Serial.flush(); }
  uint32_t ms() override { return millis(); }
  void setBaud(uint32_t baud) override { Serial.flush(); Serial.updateBaudRate(baud); }
};

static SerialIO sio;
static Engine eng(sio);

static void drawStatic() {
  auto& t = Hal::tft;
  t.fillScreen(TFT_BLACK);
  t.setTextFont(2);
  t.setTextDatum(TL_DATUM);
  t.setTextColor(TFT_CYAN, TFT_BLACK);
  t.drawString("USB SYNC", 8, 8);
  t.setTextColor(TFT_WHITE, TFT_BLACK);
  t.drawString("1) Plug the USB cable", 8, 40);
  t.drawString("   into your PC", 8, 58);
  t.drawString("2) Open MyPod Sync", 8, 90);
  t.drawString("   and press Connect", 8, 108);
  t.drawString("Hold BOOT 3s = player", 8, 296);
}

static void drawStatus(bool force) {
  static char last[5][32];
  static uint8_t lastPct = 255;
  auto& t = Hal::tft;
  char line[5][32];
  snprintf(line[0], sizeof(line[0]), "PC: %s", eng.st.linked ? "connected" : "waiting...");
  if (eng.st.total) {
    uint32_t pct = (uint32_t)((uint64_t)eng.st.got * 100 / eng.st.total);
    snprintf(line[1], sizeof(line[1]), "receiving %u%%", (unsigned)pct);
    char nm[24];
    strlcpy(nm, eng.st.name, sizeof(nm));
    snprintf(line[2], sizeof(line[2]), "%s", nm);
  } else {
    strlcpy(line[1], eng.st.linked ? "idle" : "", sizeof(line[1]));
    line[2][0] = 0;
  }
  snprintf(line[3], sizeof(line[3]), "songs received: %u", (unsigned)eng.st.songs);
  if (!eng.sdOk) strlcpy(line[4], "SD CARD MISSING!", sizeof(line[4]));
  else strlcpy(line[4], eng.st.err, 24);

  uint8_t pct = eng.st.total ? (uint8_t)((uint64_t)eng.st.got * 100 / eng.st.total) : 0;
  t.setTextFont(2);
  t.setTextDatum(TL_DATUM);
  for (int i = 0; i < 5; i++) {
    if (!force && !strcmp(line[i], last[i])) continue;
    strlcpy(last[i], line[i], sizeof(last[i]));
    t.setTextColor(i == 4 ? (eng.sdOk ? TFT_ORANGE : TFT_RED) : TFT_WHITE, TFT_BLACK);
    t.fillRect(0, 150 + i * 24, 240, 22, TFT_BLACK);
    t.drawString(line[i], 8, 150 + i * 24);
  }
  if (force || pct != lastPct) {                       // progress bar
    lastPct = pct;
    t.drawRect(8, 270, 224, 14, TFT_DARKGREY);
    t.fillRect(10, 272, 220, 10, TFT_BLACK);
    t.fillRect(10, 272, (int)(220 * pct / 100), 10, TFT_GREEN);
  }
}

void run() {
  esp_log_level_set("*", ESP_LOG_NONE);                // stray log lines would only confuse the PC
  Serial.end();
  Serial.setRxBufferSize(16384);                       // 4 KB chunks + SD hiccups never overflow
  Serial.begin(115200);
  delay(100);

  eng.sdOk = sdBegin();
  eng.onLibChanged = [] { Cfg::setLibDirty(true); };
  eng.onExit = exitToPlayer;

  pinMode(PIN_BOOT, INPUT_PULLUP);
  drawStatic();
  drawStatus(true);

  uint32_t lastUi = 0, lastSdTry = 0, bootHold = 0;
  for (;;) {
    eng.poll();

    uint32_t now = millis();
    if (digitalRead(PIN_BOOT) == LOW) {
      if (!bootHold) bootHold = now;
      if (now - bootHold > 3000) exitToPlayer();
    } else {
      bootHold = 0;
    }
    if (!eng.sdOk && now - lastSdTry > 2000) {         // card may be inserted later
      lastSdTry = now;
      eng.sdOk = sdBegin();
    }
    if (now - lastUi > 300) { lastUi = now; drawStatus(false); }
    if (!Serial.available()) delay(1);
  }
}

}  // namespace UsbSync
