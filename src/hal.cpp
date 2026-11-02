#include "hal.h"
#include "config.h"
#include "settings.h"
#include "theme.h"
#include "touch.h"
#include "player.h"
#include "ui.h"
#include <lvgl.h>
#include <esp_sleep.h>

namespace Hal {

TFT_eSPI tft = TFT_eSPI();

static bool scrOn = true;
static uint32_t lastTouch = 0, lastActive = 0;
static int curPct = 0;
static float ldrAvg = -1;

static const int DRAW_ROWS = 4;      // steps: 16 -> 8 -> 4 rows, reclaiming
                                     // RAM for the Bluetooth stack (see README)
static lv_disp_draw_buf_t drawBuf;
static lv_color_t buf1[SCR_W * DRAW_ROWS];   // single draw buffer: buf2 was
                                             // removed to give RAM back to BT
static lv_disp_drv_t dispDrv;
static lv_indev_drv_t indevDrv;
static lv_disp_t* disp = nullptr;

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

// ---------- LVGL glue ----------
static void flushCb(lv_disp_drv_t* d, const lv_area_t* a, lv_color_t* c) {
  uint32_t w = a->x2 - a->x1 + 1, h = a->y2 - a->y1 + 1;
  tft.startWrite();
  tft.setAddrWindow(a->x1, a->y1, w, h);
  tft.pushColors((uint16_t*)&c->full, w * h, true);
  tft.endWrite();
  lv_disp_flush_ready(d);
}

static void touchCb(lv_indev_drv_t*, lv_indev_data_t* d) {
  static int16_t lx = 0, ly = 0;
  static bool swallow = false;     // the touch that wakes the screen is not a click
  int16_t x, y;
  bool p = Touch::read(x, y);
  if (p) {
    lastTouch = lastActive = millis();
    if (!scrOn) { screenOn(); swallow = true; }
    if (!swallow) { lx = x; ly = y; }
  } else {
    swallow = false;
  }
  d->state = (p && !swallow) ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
  d->point.x = lx;
  d->point.y = ly;
}

void earlyInit() {
  pinMode(LED_R, OUTPUT); digitalWrite(LED_R, HIGH);
  pinMode(LED_G, OUTPUT); digitalWrite(LED_G, HIGH);
  pinMode(LED_B, OUTPUT); digitalWrite(LED_B, HIGH);
  ledcSetup(0, 5000, 8);
  ledcAttachPin(PIN_BL, 0);
  ledcWrite(0, 0);
  tft.init();
  tft.setRotation(TFT_ROT);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawCentreString(FW_NAME, SCR_W / 2, 135, 4);
  lastTouch = lastActive = millis();
  fadeTo(S.brightness, 300);
}

void lvglInit() {
  lv_init();
  lv_disp_draw_buf_init(&drawBuf, buf1, nullptr, SCR_W * DRAW_ROWS);
  lv_disp_drv_init(&dispDrv);
  dispDrv.hor_res = SCR_W;
  dispDrv.ver_res = SCR_H;
  dispDrv.flush_cb = flushCb;
  dispDrv.draw_buf = &drawBuf;
  disp = lv_disp_drv_register(&dispDrv);

  lv_indev_drv_init(&indevDrv);
  indevDrv.type = LV_INDEV_TYPE_POINTER;
  indevDrv.read_cb = touchCb;
  lv_indev_drv_register(&indevDrv);

  Th::apply();   // fills the palette + inits the LVGL theme from the saved look/accent
}

// ---------- power ----------
void applyPower() {
  setCpuFrequencyMhz(CPU_MHZ[S.cpuIdx]);
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
  UI::setActive(false);
  lv_disp_enable_invalidation(disp, false);   // no redraws while dark = less CPU/SPI work
  fadeTo(0, 200);
  scrOn = false;
}

void screenOn() {
  if (scrOn) return;
  scrOn = true;
  lastTouch = millis();
  lv_disp_enable_invalidation(disp, true);
  lv_obj_invalidate(lv_scr_act());
  UI::setActive(true);
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

}  // namespace Hal
