// XPT2046 driven by bit-banging on its own pins. The CYD wires the touch chip and
// the SD card to different pins but both would need the same hardware SPI bus
// (VSPI), so touch is software-SPI and the SD card keeps VSPI to itself.
#include "touch.h"
#include "config.h"
#include <TFT_eSPI.h>
#include <Preferences.h>

struct Cal { uint32_t magic; uint8_t swap; float xr1, xr2, yr1, yr3; };
static Cal cal;
static bool calOk = false;
static const uint32_t CAL_MAGIC = 0xCA110002;

static uint16_t xptXfer(uint8_t cmd) {
  uint16_t v = 0;
  digitalWrite(T_CS, LOW);
  for (int i = 7; i >= 0; i--) {
    digitalWrite(T_MOSI, (cmd >> i) & 1);
    digitalWrite(T_CLK, HIGH);
    digitalWrite(T_CLK, LOW);
  }
  for (int i = 0; i < 16; i++) {
    digitalWrite(T_CLK, HIGH);
    v = (v << 1) | (digitalRead(T_MISO) ? 1 : 0);
    digitalWrite(T_CLK, LOW);
  }
  digitalWrite(T_CS, HIGH);
  return (v >> 3) & 0x0FFF;
}

void Touch::begin() {
  pinMode(T_CS, OUTPUT);   digitalWrite(T_CS, HIGH);
  pinMode(T_CLK, OUTPUT);  digitalWrite(T_CLK, LOW);
  pinMode(T_MOSI, OUTPUT); digitalWrite(T_MOSI, LOW);
  pinMode(T_MISO, INPUT);
  pinMode(T_IRQ, INPUT);
}

static uint16_t median5(uint16_t* a) {
  for (int i = 1; i < 5; i++) {
    uint16_t k = a[i]; int j = i - 1;
    while (j >= 0 && a[j] > k) { a[j + 1] = a[j]; j--; }
    a[j + 1] = k;
  }
  return a[2];
}

bool Touch::readRaw(uint16_t& x, uint16_t& y) {
  if (digitalRead(T_IRQ)) return false;
  uint16_t xs[5], ys[5];
  for (int i = 0; i < 5; i++) { xs[i] = xptXfer(0xD0); ys[i] = xptXfer(0x90); }
  x = median5(xs);
  y = median5(ys);
  if (digitalRead(T_IRQ)) return false;
  return x > 80 && x < 4020 && y > 80 && y < 4020;
}

bool Touch::read(int16_t& sx, int16_t& sy) {
  uint16_t rx, ry;
  if (!readRaw(rx, ry)) return false;
  if (!calOk) { sx = SCR_W / 2; sy = SCR_H / 2; return true; }
  float ax = cal.swap ? ry : rx;
  float ay = cal.swap ? rx : ry;
  float fx = 20 + (ax - cal.xr1) * (SCR_W - 40) / (cal.xr2 - cal.xr1);
  float fy = 20 + (ay - cal.yr1) * (SCR_H - 40) / (cal.yr3 - cal.yr1);
  sx = constrain((int)fx, 0, SCR_W - 1);
  sy = constrain((int)fy, 0, SCR_H - 1);
  return true;
}

bool Touch::loadCal() {
  Preferences p;
  p.begin("touch", true);
  Cal c;
  bool ok = p.getBytes("cal", &c, sizeof(c)) == sizeof(c) && c.magic == CAL_MAGIC;
  p.end();
  if (ok) { cal = c; calOk = true; }
  return ok;
}

void Touch::forgetCal() {
  Preferences p;
  p.begin("touch", false);
  p.remove("cal");
  p.end();
}

static bool captureTap(TFT_eSPI& tft, int cx, int cy, const char* msg, uint16_t& rx, uint16_t& ry) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawCentreString("Touch calibration", SCR_W / 2, 110, 2);
  tft.drawCentreString(msg, SCR_W / 2, 140, 2);
  tft.drawCircle(cx, cy, 10, TFT_RED);
  tft.drawFastHLine(cx - 16, cy, 32, TFT_RED);
  tft.drawFastVLine(cx, cy - 16, 32, TFT_RED);
  uint16_t a, b;
  while (Touch::readRaw(a, b)) delay(10);
  delay(150);
  while (!Touch::readRaw(a, b)) delay(10);
  uint32_t sx = 0, sy = 0; int n = 0; uint32_t t0 = millis();
  while (n < 12 && millis() - t0 < 1500) {
    if (Touch::readRaw(a, b)) { sx += a; sy += b; n++; }
    delay(8);
  }
  while (Touch::readRaw(a, b)) delay(10);
  if (n < 6) return false;
  rx = sx / n; ry = sy / n;
  return true;
}

void Touch::calibrate(TFT_eSPI& tft) {
  for (;;) {
    uint16_t x1, y1, x2, y2, x3, y3;
    if (!captureTap(tft, 20, 20, "Tap the top-left target", x1, y1)) continue;
    if (!captureTap(tft, SCR_W - 20, 20, "Tap the top-right target", x2, y2)) continue;
    if (!captureTap(tft, 20, SCR_H - 20, "Tap the bottom-left target", x3, y3)) continue;
    int dx = abs((int)x2 - (int)x1), dy = abs((int)y2 - (int)y1);
    if (max(dx, dy) < 800) continue;
    Cal c;
    c.magic = CAL_MAGIC;
    c.swap = dy > dx;            // screen-X moves the raw Y axis => axes swapped
    c.xr1 = c.swap ? y1 : x1;
    c.xr2 = c.swap ? y2 : x2;
    c.yr1 = c.swap ? x1 : y1;
    c.yr3 = c.swap ? x3 : y3;
    if (fabsf(c.xr2 - c.xr1) < 500 || fabsf(c.yr3 - c.yr1) < 500) continue;
    cal = c; calOk = true;
    Preferences p;
    p.begin("touch", false);
    p.putBytes("cal", &c, sizeof(c));
    p.end();
    break;
  }
  tft.fillScreen(TFT_BLACK);
}
