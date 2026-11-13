#include "hal.h"
#include "config.h"
#include "settings.h"
#include "theme.h"
#include "touch.h"
#include "player.h"
#include "ui.h"
#include <esp_sleep.h>
#include <sys/time.h>
#include <time.h>

namespace Hal {

TFT_eSPI tft = TFT_eSPI();

static bool scrOn = true;
static uint32_t lastTouch = 0, lastActive = 0;
static int curPct = 0;
static float ldrAvg = -1;

// ---------- backlight ----------
static uint8_t duty(int pct) {
  if (pct <= 0) return 0;
  float f = pct / 100.0f;
  int d = (int)(255.0f * f * f);      // gamma-ish curve feels more linear
  return d < 2 ? 2 : d;
}

void setBrightness(int pct) {
  curPct = constrain(pct, 0, 100);
  ledcWrite(0, duty(curPct));
}

void fadeTo(int pct, uint16_t ms) {
  int from = curPct;
  int steps = max(1, (int)(ms / 10));
  for (int i = 1; i <= steps; i++) {
    setBrightness(from + (pct - from) * i / steps);
    delay(10);
  }
  setBrightness(pct);
}

static int targetPct() {
  int p = S.brightness;
  if (S.autoBright && ldrAvg >= 0) {
    float light = LDR_DARK_IS_HIGH ? 1.0f - ldrAvg / 4095.0f : ldrAvg / 4095.0f;
    light = constrain(light, 0.0f, 1.0f);
    p = (int)(p * (0.3f + 0.7f * light));
  }
  return constrain(p, 3, 100);
}

void applyBrightness() { if (scrOn) setBrightness(targetPct()); }

// ---------- touch ----------
bool readTouch(int16_t& x, int16_t& y) {
  static bool swallow = false;     // the touch that wakes the screen is not a click
  int16_t tx, ty;
  bool p = Touch::read(tx, ty);
  if (p) {
    lastTouch = lastActive = millis();
    if (!scrOn) { screenOn(); swallow = true; }
    if (swallow) return false;
    x = tx; y = ty;
    return true;
  }
  swallow = false;
  return false;
}

void earlyInit() {
  pinMode(LED_R, OUTPUT); digitalWrite(LED_R, HIGH);
  pinMode(LED_G, OUTPUT); digitalWrite(LED_G, HIGH);
  pinMode(LED_B, OUTPUT); digitalWrite(LED_B, HIGH);
  ledcSetup(0, 5000, 8);
  ledcAttachPin(PIN_BL, 0);
  ledcWrite(0, 0);
  tft.init();
#if TFT_GAMMA_FIX == 1
  // gamma fix reported for this board: select gamma curve 2, then curve 1
  tft.writecommand(0x26); tft.writedata(2);
  delay(120);
  tft.writecommand(0x26); tft.writedata(1);
#elif TFT_GAMMA_FIX == 2
  // fallback: full ST7789-style gamma tables (positive 0xE0, negative 0xE1),
  // then select gamma curve 1. Values are the common ST7789V recommended set.
  tft.writecommand(0xE0);
  tft.writedata(0xD0); tft.writedata(0x08); tft.writedata(0x11); tft.writedata(0x08);
  tft.writedata(0x0C); tft.writedata(0x15); tft.writedata(0x09); tft.writedata(0x07);
  tft.writedata(0x0E); tft.writedata(0x08); tft.writedata(0x0F); tft.writedata(0x07);
  tft.writedata(0x07); tft.writedata(0x0F);
  tft.writecommand(0xE1);
  tft.writedata(0xD0); tft.writedata(0x08); tft.writedata(0x11); tft.writedata(0x08);
  tft.writedata(0x0C); tft.writedata(0x15); tft.writedata(0x09); tft.writedata(0x07);
  tft.writedata(0x0E); tft.writedata(0x08); tft.writedata(0x0F); tft.writedata(0x07);
  tft.writedata(0x07); tft.writedata(0x0F);
  tft.writecommand(0x26); tft.writedata(1);
#endif
  tft.setRotation(TFT_ROT);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawCentreString(FW_NAME, SCR_W / 2, 135, 4);
  lastTouch = lastActive = millis();
  fadeTo(S.brightness, 300);
}

// ---------- power ----------
void applyPower() {
  // MP3 decode + Bluetooth SBC encode need headroom: below 240 MHz the decoder
  // can fall behind for a moment and the music cuts out. Floor the clock at 240.
  setCpuFrequencyMhz(CPU_MHZ[S.cpuIdx < 2 ? 2 : S.cpuIdx]);
  if (!S.statusLed) {                      // setting off: all channels dark
    digitalWrite(LED_R, HIGH);
    digitalWrite(LED_G, HIGH);
    digitalWrite(LED_B, HIGH);
  }
  applyBrightness();
}

bool isScreenOn() { return scrOn; }

void screenOff() {
  if (!scrOn) return;
  UI::setActive(false);                    // UI stops drawing while dark
  fadeTo(0, 200);
  scrOn = false;
}

void screenOn() {
  if (scrOn) return;
  scrOn = true;
  lastTouch = millis();
  UI::setActive(true);                     // repaints the current screen
  fadeTo(targetPct(), 150);
}

void deepSleep() {
  Cfg::save();
  UI::setActive(false);
  fadeTo(0, 200);
  digitalWrite(LED_B, HIGH);
  uint16_t a, b;
  while (Touch::readRaw(a, b)) delay(20);      // finger must be off, or we'd wake instantly
  delay(200);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_36, 0); // touch IRQ is active low
  esp_deep_sleep_start();
}

void tick() {
  uint32_t now = millis();
  if (Player::active()) lastActive = now;

  if (scrOn) {
    uint16_t to = TIMEOUT_SEC[S.timeoutIdx];
    if (to && now - lastTouch > to * 1000UL) screenOff();

    static uint32_t lastLdr = 0;
    if (S.autoBright && now - lastLdr > 400) {
      lastLdr = now;
      float raw = analogRead(PIN_LDR);
      ldrAvg = ldrAvg < 0 ? raw : ldrAvg + (raw - ldrAvg) * 0.2f;
      setBrightness(targetPct());
    }
  }

  static uint32_t lastLed = 0;
  if (now - lastLed > 500) {
    lastLed = now;
    if (!S.statusLed || !scrOn) {            // LED off: every channel dark
      digitalWrite(LED_R, HIGH);
      digitalWrite(LED_G, HIGH);
      digitalWrite(LED_B, HIGH);
    } else if (Player::btConnected()) {      // headphones connected: solid blue
      digitalWrite(LED_B, LOW);
    } else {                                 // searching: slow blue blink
      digitalWrite(LED_B, (now / 2000) % 2 ? HIGH : LOW);
    }
  }

  uint16_t po = POWEROFF_MIN[S.powerOffIdx];
  if (po && !Player::active() && now - lastActive > po * 60000UL) deepSleep();
}

// ---------- clock ----------
bool clockValid() { return time(nullptr) > 1700000000L; }

void clockGet(int& h, int& m) {
  time_t t = time(nullptr);
  struct tm tmv;
  localtime_r(&t, &tmv);
  h = tmv.tm_hour; m = tmv.tm_min;
}

void clockSet(int h, int m) {
  struct tm tmv;
  memset(&tmv, 0, sizeof(tmv));
  tmv.tm_year = 126; tmv.tm_mon = 0; tmv.tm_mday = 1;     // 2026-01-01, day is irrelevant
  tmv.tm_hour = constrain(h, 0, 23); tmv.tm_min = constrain(m, 0, 59);
  struct timeval tv = {mktime(&tmv), 0};
  settimeofday(&tv, nullptr);
}

// ---------- battery ----------
int batteryPercent() {
#if BAT_ADC_PIN >= 0
  static float mvAvg = -1;
  float mv = analogReadMilliVolts(BAT_ADC_PIN) * BAT_DIV;
  mvAvg = mvAvg < 0 ? mv : mvAvg + (mv - mvAvg) * 0.1f;
  return constrain((int)((mvAvg - 3300.0f) * 100.0f / 850.0f), 0, 100);   // 3.30 V..4.15 V
#else
  return -1;
#endif
}

}  // namespace Hal
