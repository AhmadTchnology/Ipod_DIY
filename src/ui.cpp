// MyPod UI — three screens (Now Playing, Library, Settings) + headphone picker.
// Layout of the Now Playing screen follows the "KLIK MP3 Player" Lopaka design
// (240x320): clock + status on top, 200x200 art, vertical volume bar, title,
// artist, progress bar, times, play state, mode and the next track.
#include "ui.h"
#include "config.h"
#include "settings.h"
#include "theme.h"
#include "hal.h"
#include "library.h"
#include "player.h"
#include "touch.h"
#include "receiver.h"
#include <TFT_eSPI.h>

#define T     (Hal::tft)
#define C_BG  (Th::bg)
#define C_SUR (Th::surf)
#define C_SU2 (Th::surf2)
#define C_TXT (Th::txt)
#define C_DIM (Th::dim)
#define C_ACC (Th::acc)

// ------------------------------------------------------------------ layout (Lopaka)
static const int ART_X = 20, ART_Y = 20, ART_W = 200, ART_H = 200;
static const int VOL_X = 225, VOL_Y = 20, VOL_W = 10, VOL_H = 200;
static const int TITLE_X = 20, TITLE_Y = 230, TITLE_W = 200;
static const int ARTIST_Y = 250;
static const int PROG_X = 20, PROG_Y = 263, PROG_W = 200, PROG_H = 10;
static const int TIME_Y = 279;
static const int ICON_Y = 277;
static const int MODE_Y = 291;
static const int NEXT_Y = 303;

enum Screen : uint8_t { SC_NOW, SC_LIB, SC_SET, SC_PICK };
static Screen scr = SC_NOW;
static bool active = true;
static bool needFull = true;

// ------------------------------------------------------------------ small helpers
static void txt(TFT_eSPI& g, const char* s, int x, int y, uint8_t font, uint8_t size,
                uint16_t fg, uint16_t bg, uint8_t datum = TL_DATUM, int pad = 0) {
  g.setTextFont(font);
  g.setTextSize(size);
  g.setTextColor(fg, bg);
  g.setTextDatum(datum);
  g.setTextPadding(pad);
  g.drawString(s, x, y);
  g.setTextPadding(0);
}

static void clip(char* dst, const char* src, size_t maxChars) {   // copy, ".." when cut
  size_t n = strlen(src);
  if (n <= maxChars) { strcpy(dst, src); return; }
  if (maxChars < 3) { dst[0] = 0; return; }
  memcpy(dst, src, maxChars - 2);
  dst[maxChars - 2] = '.'; dst[maxChars - 1] = '.'; dst[maxChars] = 0;
}

// shorten to fit maxPx for the given built-in font, ending in ".." when cut
static void fitText(TFT_eSPI& g, const char* s, uint8_t font, int maxPx, char* out, size_t outN) {
  g.setTextFont(font);
  g.setTextSize(1);
  strlcpy(out, s, outN);
  if (g.textWidth(out) <= maxPx) return;
  for (int n = (int)strlen(s); n > 0; n--) {
    snprintf(out, outN, "%.*s..", n, s);
    if (g.textWidth(out) <= maxPx) return;
  }
}

static uint16_t hsv565(int h, int s, int v) {       // h 0..359, s,v 0..255
  int region = h / 60, rem = (h % 60) * 255 / 60;
  int p = v * (255 - s) / 255;
  int q = v * (255 - s * rem / 255) / 255;
  int t = v * (255 - s * (255 - rem) / 255) / 255;
  int r, g, b;
  switch (region) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
  }
  return Th::rgb(((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
}

static void fmtTime(char* o, size_t n, uint32_t s) {
  snprintf(o, n, "%02u:%02u", (unsigned)(s / 60 % 100), (unsigned)(s % 60));
}

// ------------------------------------------------------------------ toast + dialog
static char toastMsg[40];
static uint32_t toastUntil = 0;
static bool toastShown = false;

static void drawToast() {
  int w = (int)strlen(toastMsg) * 6 + 24;
  if (w > 224) w = 224;
  int x = (SCR_W - w) / 2;
  T.fillRoundRect(x, 284, w, 24, 6, C_SU2);
  T.drawRoundRect(x, 284, w, 24, 6, C_ACC);
  txt(T, toastMsg, SCR_W / 2, 296, 1, 1, C_TXT, C_SU2, MC_DATUM);
}

static void toast(const char* m, uint32_t ms = 2000) {
  strlcpy(toastMsg, m, sizeof(toastMsg));
  toastUntil = millis() + ms;
  toastShown = true;
  drawToast();
}

enum DlgAct : uint8_t { DLG_NONE, DLG_PAIR, DLG_WIFI, DLG_RECAL };
static struct { bool open; DlgAct act; const char* title; const char* l1; const char* l2; } dlg;

static bool dlgDrawPending = false;

static void openDlg(DlgAct a, const char* title, const char* l1, const char* l2) {
  dlg.open = true; dlg.act = a; dlg.title = title; dlg.l1 = l1; dlg.l2 = l2;
  dlgDrawPending = true;
}

static void drawDlg() {
  T.fillRoundRect(16, 92, 208, 136, 10, C_SUR);
  T.drawRoundRect(16, 92, 208, 136, 10, C_ACC);
  txt(T, dlg.title, 28, 104, 2, 1, C_TXT, C_SUR);
  txt(T, dlg.l1, 28, 132, 1, 1, C_DIM, C_SUR);
  txt(T, dlg.l2, 28, 146, 1, 1, C_DIM, C_SUR);
  T.fillRoundRect(28, 186, 88, 30, 6, C_SU2);
  txt(T, "Cancel", 72, 201, 2, 1, C_TXT, C_SU2, MC_DATUM);
  T.fillRoundRect(124, 186, 88, 30, 6, C_ACC);
  txt(T, "Restart", 168, 201, 2, 1, C_BG, C_ACC, MC_DATUM);
}

static void runDlg() {
  switch (dlg.act) {
    case DLG_PAIR:  S.pairNext = 1; Cfg::save(); break;
    case DLG_WIFI:  Receiver::setEnabled(true); break;
    case DLG_RECAL: Touch::forgetCal(); Cfg::save(); break;
    default: return;
  }
  T.fillScreen(C_BG);
  txt(T, "Restarting...", SCR_W / 2, SCR_H / 2, 2, 1, C_TXT, C_BG, MC_DATUM);
  delay(300);
  ESP.restart();
}

// ------------------------------------------------------------------ gesture tracking
struct Gest {
  bool down = false;
  int x0 = 0, y0 = 0, x = 0, y = 0;
  uint32_t t0 = 0, tl = 0;
  float vy = 0;               // px/s, smoothed
  bool moved = false;
  uint8_t miss = 0;
};
static Gest G;
static const int TAP_SLOP = 10;

// ------------------------------------------------------------------ generic scrolling list
struct ListView {
  int rowH = 40, count = 0;
  float pos = 0, vel = 0;
  int maxPos = 0;
  int pressed = -1;
  bool dirty = true;
  bool kinetic = false;
  uint32_t lastStep = 0, lastDraw = 0;
  void setCount(int n, int h) { count = n; rowH = h; }
  void layout(int viewH) { maxPos = max(0, count * rowH - viewH); pos = constrain(pos, 0.0f, (float)maxPos); }
  int rowAt(int y, int y0) const { int r = ((int)pos + y - y0) / rowH; return (y < y0 || r < 0 || r >= count) ? -1 : r; }
};

static TFT_eSprite* rowSpr = nullptr;
static bool sprTried = false;

static void sprAcquire() {
  if (rowSpr || sprTried) return;
  sprTried = true;
  if (ESP.getFreeHeap() < 48000 || ESP.getMaxAllocHeap() < 24000) return;   // tight on RAM: draw rows directly
  TFT_eSprite* s = new TFT_eSprite(&T);
  s->setColorDepth(16);
  if (s->createSprite(SCR_W, 40)) rowSpr = s; else delete s;
}

static void sprRelease() {
  if (rowSpr) { rowSpr->deleteSprite(); delete rowSpr; rowSpr = nullptr; }
  sprTried = false;
}

typedef void (*RowFn)(TFT_eSPI& g, int y, int idx, bool pressed);

static void renderList(ListView& L, int y0, int viewH, RowFn fn) {
  L.layout(viewH);
  sprAcquire();
  T.setViewport(0, y0, SCR_W, viewH, false);
  int first = (int)L.pos / L.rowH;
  int offs = (int)L.pos % L.rowH;
  int y = y0 - offs;
  for (int i = first; i < L.count && y < y0 + viewH; i++, y += L.rowH) {
    bool pr = (i == L.pressed);
    if (rowSpr) { fn(*rowSpr, 0, i, pr); rowSpr->pushSprite(0, y); }
    else        { fn(T, y, i, pr); }
  }
  if (y < y0 + viewH) T.fillRect(0, y, SCR_W, y0 + viewH - y, C_BG);   // short list: clear below
  T.resetViewport();
  L.dirty = false;
}

// kinetic scrolling: returns true while still moving
static bool stepKinetic(ListView& L, uint32_t now, int viewH) {
  if (!L.kinetic) return false;
  float dt = (now - L.lastStep) / 1000.0f;
  if (dt <= 0) return true;
  L.lastStep = now;
  if (dt > 0.08f) dt = 0.08f;
  L.pos -= L.vel * dt;
  L.vel *= expf(-dt * 3.2f);                // friction
  L.layout(viewH);
  if (L.pos <= 0.0f || L.pos >= L.maxPos) L.vel = 0;
  if (fabsf(L.vel) < 25.0f) { L.vel = 0; L.kinetic = false; }
  L.dirty = true;
  return L.kinetic;
}

// ================================================================== NOW PLAYING
static char npTitle[64] = "";
static char npArtist[48] = "";
static char npNext[40] = "";
static uint32_t npSeed = 0;
static int npLastCur = -2;
static uint32_t npLastSerial = 0xFFFFFFFF;
static int npTitleW = 0;
static float marq = 0;
static uint32_t marqT = 0, marqHold = 0;

static int c_prog = -1, c_vol = -1, c_clock = -1, c_state = -1, c_mode = -1, c_bt = -2;
static uint32_t c_el = 0xFFFFFFFF, c_dur = 0xFFFFFFFF;
static bool dragSeek = false, dragVol = false;
static float seekPrev = 0;
static uint32_t npLastTick = 0;

static int modeIndex() { return S.shuffle ? 3 : (S.repeat == 2 ? 2 : (S.repeat == 1 ? 1 : 0)); }
static const char* const MODE_TXT[4] = {"Play in order", "Repeat all", "Repeat one", "Shuffle"};
static void setMode(int m) {
  switch (m & 3) {
    case 0: Player::setShuffle(false); Player::setRepeat(0); break;
    case 1: Player::setShuffle(false); Player::setRepeat(1); break;
    case 2: Player::setShuffle(false); Player::setRepeat(2); break;
    default: Player::setShuffle(true);  Player::setRepeat(1); break;
  }
}

static void drawIconList(int x, int y, uint16_t c) {            // 14x10 "menu" glyph
  for (int i = 0; i < 3; i++) T.fillRect(x, y + i * 4, 14, 2, c);
}
static void drawIconGear(int x, int y, uint16_t c) {            // 12x12 gear
  T.fillCircle(x + 6, y + 6, 4, c);
  T.fillRect(x + 5, y, 2, 12, c);
  T.fillRect(x, y + 5, 12, 2, c);
  T.fillCircle(x + 6, y + 6, 1, C_BG);
}

static void npTop() {
  T.fillRect(0, 0, SCR_W, 18, C_BG);
  drawIconList(8, 5, C_DIM);
  drawIconGear(34, 3, C_DIM);
  char b[16];
  if (Hal::clockValid()) { int h, m; Hal::clockGet(h, m); snprintf(b, sizeof(b), "%02d:%02d", h, m); c_clock = h * 60 + m; }
  else { strcpy(b, "--:--"); c_clock = -1; }
  txt(T, b, 121, 6, 1, 1, C_TXT, C_BG, TC_DATUM);
  int pct = Hal::batteryPercent();
  if (pct >= 0) snprintf(b, sizeof(b), "Batt: %d%%", pct);
  else snprintf(b, sizeof(b), "BT: %s", Player::btConnected() ? "ok" : "--");
  txt(T, b, 235, 6, 1, 1, C_TXT, C_BG, TR_DATUM);
  c_bt = Player::btConnected();
}

static void drawArt(int xoff) {
  T.setViewport(ART_X, ART_Y, ART_W, ART_H, false);
  int hue = npSeed % 360;
  uint16_t c1 = hsv565(hue, 150, 170), c2 = hsv565((hue + 45) % 360, 190, 55);
  for (int i = 0; i < 50; i++)
    T.fillRect(ART_X + xoff, ART_Y + i * 4, ART_W, 4, Th::blend(c1, c2, (uint8_t)(i * 255 / 49)));
  uint16_t nc = Th::blend(c1, 0xFFFF, 175);
  int cx = ART_X + xoff + 100, cy = ART_Y + 86;
  T.fillCircle(cx - 24, cy + 34, 14, nc);
  T.fillCircle(cx + 30, cy + 24, 14, nc);
  T.fillRect(cx - 12, cy - 36, 6, 70, nc);
  T.fillRect(cx + 42, cy - 46, 6, 70, nc);
  T.fillTriangle(cx - 12, cy - 36, cx + 48, cy - 46, cx + 48, cy - 30, nc);
  T.fillTriangle(cx - 12, cy - 36, cx - 12, cy - 20, cx + 48, cy - 30, nc);
  txt(T, "Album Unavailable", cx, ART_Y + 178, 1, 1, Th::blend(c1, 0xFFFF, 130), c2, MC_DATUM);
  T.resetViewport();
  T.drawRect(ART_X, ART_Y, ART_W, ART_H, C_TXT);
}

static void npSlideArt(int dir) {                    // dir +1: new art enters from the right
  if (!S.animations) { drawArt(0); return; }
  static const int steps[] = {110, 170, 196, 200};
  for (int i = 0; i < 4; i++) {
    int off = dir * (ART_W - steps[i]);
    drawArt(off);
  }
}

static void npVol() {
  int h = (VOL_H - 2) * S.volume / 100;
  T.drawRect(VOL_X, VOL_Y, VOL_W, VOL_H, C_TXT);
  T.fillRect(VOL_X + 1, VOL_Y + 1, VOL_W - 2, VOL_H - 2 - h, C_BG);
  T.fillRect(VOL_X + 1, VOL_Y + VOL_H - 1 - h, VOL_W - 2, h, C_ACC);
  c_vol = S.volume;
}

static void npArtistLine() {
  char b[36];
  clip(b, npArtist[0] ? npArtist : "Unknown artist", 33);
  txt(T, b, TITLE_X, ARTIST_Y, 1, 1, C_DIM, C_BG, TL_DATUM, TITLE_W);
}

static void npTitleLine(bool forceStatic) {
  npTitleW = (int)strlen(npTitle) * 12;
  if (npTitleW <= TITLE_W) {
    txt(T, npTitle, TITLE_X, TITLE_Y, 1, 2, C_TXT, C_BG, TL_DATUM, TITLE_W);
    marq = 0;
    return;
  }
  if (forceStatic) marq = 0;
  // scrolling: render only the ~18 characters that are inside the window
  static char win[24];
  int total = npTitleW + 36;                       // title + 3-char gap
  int off = (int)marq % total;
  int ci = off / 12, sub = off % 12;
  int len = (int)strlen(npTitle);
  for (int i = 0; i < 18; i++) {
    int k = (ci + i) % (len + 3);
    win[i] = k < len ? npTitle[k] : ' ';
  }
  win[18] = 0;
  T.setViewport(TITLE_X, TITLE_Y, TITLE_W, 16, false);
  txt(T, win, TITLE_X - sub, TITLE_Y, 1, 2, C_TXT, C_BG);
  T.resetViewport();
}

static void npProgress(float frac) {
  int fw = (int)((PROG_W - 2) * constrain(frac, 0.0f, 1.0f));
  T.drawRect(PROG_X, PROG_Y, PROG_W, PROG_H, C_TXT);
  T.fillRect(PROG_X + 1, PROG_Y + 1, fw, PROG_H - 2, C_ACC);
  T.fillRect(PROG_X + 1 + fw, PROG_Y + 1, PROG_W - 2 - fw, PROG_H - 2, C_BG);
  c_prog = fw;
}

static void npTimes(uint32_t el, uint32_t dur) {
  char b[8];
  fmtTime(b, sizeof(b), el);
  txt(T, b, PROG_X, TIME_Y, 1, 1, C_TXT, C_BG, TL_DATUM, 32);
  if (dur) fmtTime(b, sizeof(b), dur); else strcpy(b, "--:--");
  txt(T, b, PROG_X + PROG_W, TIME_Y, 1, 1, C_TXT, C_BG, TR_DATUM, 32);
  c_el = el; c_dur = dur;
}

static void npControls() {                          // prev | pause/play | next
  T.fillRect(80, 275, 80, 16, C_BG);                 // stays above the mode line (y=291)
  T.fillRect(108, 275, 24, 22, C_BG);
  // prev (centre x = 90)
  T.fillRect(84, 277, 2, 12, C_TXT);
  T.fillTriangle(97, 277, 97, 288, 88, 283, C_TXT);
  // next (centre x = 150)
  T.fillRect(154, 277, 2, 12, C_TXT);
  T.fillTriangle(143, 277, 143, 288, 152, 283, C_TXT);
  // play state
  if (Player::active()) {
    T.fillRect(113, ICON_Y, 5, 18, C_ACC);
    T.fillRect(122, ICON_Y, 5, 18, C_ACC);
    c_state = 1;
  } else {
    T.fillTriangle(114, ICON_Y, 114, ICON_Y + 17, 128, ICON_Y + 9, C_ACC);
    c_state = 0;
  }
}

static void npModeLine() {
  txt(T, MODE_TXT[modeIndex()], 20, MODE_Y, 1, 1, C_ACC, C_BG, TL_DATUM, 90);
  c_mode = modeIndex();
}

static void npNextLine() {
  char b[32];
  txt(T, "Next Track:", 20, NEXT_Y, 1, 1, C_DIM, C_BG);
  fitText(T, npNext[0] ? npNext : "-", 1, 142, b, sizeof(b));
  txt(T, b, 92, NEXT_Y, 1, 1, C_TXT, C_BG, TL_DATUM, 144);
}

static void npFetchMeta() {
  Player::meta(npTitle, sizeof(npTitle), npArtist, sizeof(npArtist));
  int cur = Player::current();
  if (cur < 0) {
    strlcpy(npTitle, "Not playing", sizeof(npTitle));
    strlcpy(npArtist, !Library::sdOk() ? "No SD card detected"
                    : (Library::count() ? "Tap the menu icon" : "No songs in /music"),
            sizeof(npArtist));
    npNext[0] = 0;
  } else {
    int ni = Player::nextIndex();
    if (ni >= 0) { char a[8]; Library::displayName(ni, npNext, sizeof(npNext), a, sizeof(a)); }
    else strlcpy(npNext, S.shuffle ? "Shuffle" : "End of list", sizeof(npNext));
  }
  uint32_t h = 5381;
  for (const char* p = npTitle; *p; p++) h = h * 33 + (uint8_t)*p;
  npSeed = h;
}

static void npFull() {
  T.fillScreen(C_BG);
  npTop();
  drawArt(0);
  npVol();
  npTitleLine(true);
  npArtistLine();
  npProgress(Player::fraction());
  npTimes(Player::loaded() ? (uint32_t)Player::position() : 0, Player::loaded() ? Player::duration() : 0);
  npControls();
  npModeLine();
  npNextLine();
}

static void npTick(uint32_t now) {
  int cur = Player::current();
  uint32_t ser = Player::serial();
  if (cur != npLastCur || ser != npLastSerial) {
    int dir = (npLastCur >= 0 && cur >= 0 && cur < npLastCur) ? -1 : 1;
    bool trackChanged = (cur != npLastCur);
    npLastCur = cur; npLastSerial = ser;
    npFetchMeta();
    if (trackChanged) npSlideArt(dir); else drawArt(0);
    npTitleLine(true); npArtistLine(); npNextLine();
    marqHold = now + 1200;
  }
  if (now - npLastTick < 50) return;
  uint32_t dt = now - npLastTick;
  npLastTick = now;

  // marquee
  if (npTitleW > TITLE_W && now > marqHold) {
    marq += dt * 0.032f;                            // ~32 px/s
    if (marq >= npTitleW + 36) { marq = 0; marqHold = now + 1200; }
    npTitleLine(false);
  }

  // progress / times
  float frac = dragSeek ? seekPrev : Player::fraction();
  int fw = (int)((PROG_W - 2) * constrain(frac, 0.0f, 1.0f));
  if (fw != c_prog) npProgress(frac);
  uint32_t el = Player::loaded() ? (uint32_t)(dragSeek && Player::duration() ? seekPrev * Player::duration() : Player::position()) : 0;
  uint32_t du = Player::loaded() ? Player::duration() : 0;
  if (el != c_el || du != c_dur) npTimes(el, du);

  if (S.volume != c_vol && !dragVol) npVol();
  if ((int)Player::active() != c_state) npControls();
  if (modeIndex() != c_mode) { npModeLine(); npFetchMeta(); npNextLine(); }
  int mins = -1;
  if (Hal::clockValid()) { int h, m; Hal::clockGet(h, m); mins = h * 60 + m; }
  if (mins != c_clock || (int)Player::btConnected() != c_bt) npTop();

  const char* e = Player::error();
  if (e[0]) toast(e, 2600);
}

// ---- Now Playing touch ----
static bool inRect(int x, int y, int rx, int ry, int rw, int rh) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void setVolFromY(int y) {
  int v = (VOL_Y + VOL_H - 1 - y) * 100 / (VOL_H - 2);
  Player::setVolume((uint8_t)constrain(v, 0, 100));
  npVol();
}

static void go(Screen s);

static void npPress(int x, int y) {
  dragSeek = dragVol = false;
  if (x >= 212 && y >= 12 && y <= 228) { dragVol = true; setVolFromY(y); return; }
  if (inRect(x, y, 10, 252, 220, 24)) {
    dragSeek = true;
    seekPrev = constrain((x - PROG_X) / (float)PROG_W, 0.0f, 1.0f);
    return;
  }
}

static void npMove(int x, int y) {
  if (dragVol) { setVolFromY(y); return; }
  if (dragSeek) seekPrev = constrain((x - PROG_X) / (float)PROG_W, 0.0f, 1.0f);
}

static void npRelease(int x, int y, bool tap) {
  if (dragVol) { dragVol = false; return; }
  if (dragSeek) {
    dragSeek = false;
    if (Player::loaded()) Player::seek(seekPrev);
    return;
  }
  int dx = x - G.x0, dy = y - G.y0;
  uint32_t dt = millis() - G.t0;
  // swipe on the art: next / previous
  if (inRect(G.x0, G.y0, ART_X, ART_Y, ART_W, ART_H) && abs(dx) > 45 && abs(dy) < 45 && dt < 700) {
    if (dx < 0) Player::next(); else Player::prev();
    return;
  }
  if (!tap) return;
  if (inRect(x, y, 0, 0, 30, 24)) { go(SC_LIB); return; }
  if (inRect(x, y, 30, 0, 36, 24)) { go(SC_SET); return; }
  if (inRect(x, y, ART_X, ART_Y, ART_W, ART_H)) { Player::toggle(); return; }
  if (inRect(x, y, 64, 270, 40, 21)) { Player::prev(); return; }
  if (inRect(x, y, 104, 270, 32, 21)) { Player::toggle(); return; }
  if (inRect(x, y, 136, 270, 40, 21)) { Player::next(); return; }
  if (inRect(x, y, 10, 291, 100, 10)) { setMode(modeIndex() + 1); return; }
}

// ================================================================== LIBRARY
static ListView lib;
static const int LIB_HDR = 32, LIB_ROWH = 40;
static const int LIB_LIST_H = SCR_H - LIB_HDR;

struct NameEnt { int idx; char title[36]; char artist[28]; };
static NameEnt nameCache[12];

static void nameCacheClear() { for (auto& e : nameCache) e.idx = -1; }

static const NameEnt& nameFor(int idx) {
  NameEnt& e = nameCache[idx % 12];
  if (e.idx != idx) { e.idx = idx; Library::displayName(idx, e.title, sizeof(e.title), e.artist, sizeof(e.artist)); }
  return e;
}

static void libRow(TFT_eSPI& g, int y, int idx, bool pressed) {
  const NameEnt& e = nameFor(idx);
  bool cur = (idx == Player::current());
  uint16_t bgc = pressed ? C_SU2 : C_BG;
  g.fillRect(0, y, SCR_W, LIB_ROWH, bgc);
  if (cur) g.fillRect(0, y + 4, 3, LIB_ROWH - 8, C_ACC);
  char tb[44], ab[36];
  fitText(g, e.title, 2, 200, tb, sizeof(tb));
  fitText(g, e.artist[0] ? e.artist : "Unknown artist", 1, 200, ab, sizeof(ab));
  txt(g, tb, 12, y + 4, 2, 1, cur ? C_ACC : C_TXT, bgc);
  txt(g, ab, 12, y + 25, 1, 1, C_DIM, bgc);
  if (cur) {                                         // tiny equaliser glyph
    uint16_t c = C_ACC;
    g.fillRect(220, y + 22, 3, 8, c); g.fillRect(225, y + 16, 3, 14, c); g.fillRect(230, y + 19, 3, 11, c);
  }
  g.drawFastHLine(12, y + LIB_ROWH - 1, SCR_W - 24, C_SUR);
}

static void libHeader() {
  T.fillRect(0, 0, SCR_W, LIB_HDR, C_BG);
  T.fillTriangle(16, 16, 24, 9, 24, 23, C_ACC);                  // back chevron
  txt(T, "Library", 38, 8, 2, 1, C_TXT, C_BG);
  char b[20]; snprintf(b, sizeof(b), "%d songs", Library::count());
  txt(T, b, 232, 12, 1, 1, C_DIM, C_BG, TR_DATUM);
  T.drawFastHLine(0, LIB_HDR - 1, SCR_W, C_SU2);
}

static void libFull() {
  T.fillScreen(C_BG);
  libHeader();
  if (Library::count() == 0) {
    txt(T, Library::sdOk() ? "No songs found" : "No SD card", SCR_W / 2, 140, 2, 1, C_TXT, C_BG, MC_DATUM);
    txt(T, "Copy .mp3 files to /music", SCR_W / 2, 164, 1, 1, C_DIM, C_BG, MC_DATUM);
    return;
  }
  lib.dirty = true;
}

static void libTick(uint32_t now) {
  stepKinetic(lib, now, LIB_LIST_H);
  if (lib.dirty && Library::count() && now - lib.lastDraw >= 18) {
    renderList(lib, LIB_HDR, LIB_LIST_H, libRow);
    lib.lastDraw = now;
  }
}

static void libPress(int x, int y) {
  lib.kinetic = false; lib.vel = 0;
  if (y >= LIB_HDR) { lib.pressed = lib.rowAt(y, LIB_HDR); lib.dirty = true; }
}

static void libMove(int x, int y) {
  int dy = y - G.y0;
  if (!G.moved && abs(dy) > TAP_SLOP) { G.moved = true; lib.pressed = -1; }
  static float startPos = 0;
  static bool init = false;
  if (!init || !G.moved) { startPos = lib.pos; init = true; }
  if (G.moved) {
    lib.pos = constrain(startPos - dy, 0.0f, (float)lib.maxPos);
    lib.dirty = true;
  }
}

static void libRelease(int x, int y, bool tap) {
  int pr = lib.pressed;
  lib.pressed = -1; lib.dirty = true;
  if (tap) {
    if (y < LIB_HDR && x < 80) { go(SC_NOW); return; }
    if (pr >= 0) {
      if (!(pr == Player::current() && Player::loaded())) Player::play(pr);
      go(SC_NOW);
      return;
    }
  } else if (fabsf(G.vy) > 150) {
    lib.vel = constrain(G.vy, -2500.0f, 2500.0f);
    lib.kinetic = true;
    lib.lastStep = millis();
  }
}

// ================================================================== SETTINGS
enum IType : uint8_t { I_SECTION, I_TOGGLE, I_CYCLE, I_SLIDER, I_ACTION, I_INFO, I_COLOR };
enum SId : uint8_t {
  SID_NONE, SID_SHUFFLE, SID_REPEAT, SID_VOLLIM, SID_BRIGHT, SID_AUTOB, SID_TIMEOUT, SID_SCROFF,
  SID_CPU, SID_LED, SID_ANIM, SID_POWEROFF, SID_SLEEP, SID_LOOK, SID_ACCENT, SID_CLKH, SID_CLKM,
  SID_BT, SID_PAIR, SID_WIFI, SID_RESCAN, SID_RECAL, SID_INFO1, SID_INFO2
};
struct Item { IType t; SId id; const char* label; };
static const Item ITEMS[] = {
  {I_SECTION, SID_NONE, "PLAYBACK"},
  {I_TOGGLE,  SID_SHUFFLE,  "Shuffle"},
  {I_CYCLE,   SID_REPEAT,   "Repeat"},
  {I_SLIDER,  SID_VOLLIM,   "Volume limit"},
  {I_SECTION, SID_NONE, "DISPLAY"},
  {I_SLIDER,  SID_BRIGHT,   "Brightness"},
  {I_TOGGLE,  SID_AUTOB,    "Auto brightness"},
  {I_CYCLE,   SID_TIMEOUT,  "Screen off after"},
  {I_ACTION,  SID_SCROFF,   "Turn screen off now"},
  {I_SECTION, SID_NONE, "POWER SAVING"},
  {I_CYCLE,   SID_CPU,      "CPU speed"},
  {I_TOGGLE,  SID_LED,      "Status LED"},
  {I_TOGGLE,  SID_ANIM,     "Animations"},
  {I_CYCLE,   SID_POWEROFF, "Auto power-off"},
  {I_ACTION,  SID_SLEEP,    "Sleep now (touch wakes)"},
  {I_SECTION, SID_NONE, "THEME"},
  {I_CYCLE,   SID_LOOK,     "Look"},
  {I_COLOR,   SID_ACCENT,   "Accent colour"},
  {I_SECTION, SID_NONE, "CLOCK (tap left / right)"},
  {I_CYCLE,   SID_CLKH,     "Hour"},
  {I_CYCLE,   SID_CLKM,     "Minute"},
  {I_SECTION, SID_NONE, "BLUETOOTH"},
  {I_INFO,    SID_BT,       "Headphones"},
  {I_ACTION,  SID_PAIR,     "Pair new headphones..."},
  {I_SECTION, SID_NONE, "WIFI RECEIVER"},
  {I_ACTION,  SID_WIFI,     "Receive songs via Telegram..."},
  {I_SECTION, SID_NONE, "SYSTEM"},
  {I_ACTION,  SID_RESCAN,   "Rescan music library"},
  {I_ACTION,  SID_RECAL,    "Recalibrate touch..."},
  {I_INFO,    SID_INFO1,    "Tracks"},
  {I_INFO,    SID_INFO2,    "Free RAM"},
};
static const int N_ITEMS = sizeof(ITEMS) / sizeof(ITEMS[0]);
static const int SET_HDR = 32, SET_ROWH = 32;
static const int SET_LIST_H = SCR_H - SET_HDR;
static ListView setv;
static int sliderRow = -1;

static const char* const TIMEOUT_TXT[5] = {"Never", "15 s", "30 s", "1 min", "2 min"};
static const char* const CPU_TXT[3]     = {"80 MHz", "160 MHz", "240 MHz"};
static const char* const PO_TXT[4]      = {"Never", "5 min", "10 min", "30 min"};
static const char* const REP_TXT[3]     = {"Off", "All", "One"};

static bool toggleVal(SId id) {
  switch (id) {
    case SID_SHUFFLE: return S.shuffle;
    case SID_AUTOB:   return S.autoBright;
    case SID_LED:     return S.statusLed;
    case SID_ANIM:    return S.animations;
    default: return false;
  }
}

static void sliderRange(SId id, int& mn, int& mx, int& val) {
  if (id == SID_VOLLIM) { mn = 20; mx = 100; val = S.volLimit; }
  else                  { mn = 5;  mx = 100; val = S.brightness; }
}

static void valueText(SId id, char* b, size_t n) {
  b[0] = 0;
  switch (id) {
    case SID_REPEAT:   strlcpy(b, REP_TXT[S.repeat % 3], n); break;
    case SID_TIMEOUT:  strlcpy(b, TIMEOUT_TXT[S.timeoutIdx % 5], n); break;
    case SID_CPU:      strlcpy(b, CPU_TXT[S.cpuIdx % 3], n); break;
    case SID_POWEROFF: strlcpy(b, PO_TXT[S.powerOffIdx % 4], n); break;
    case SID_LOOK:     strlcpy(b, Th::LOOK_NAME[Th::look()], n); break;
    case SID_ACCENT:   strlcpy(b, Th::ACC_NAME[Th::accent()], n); break;
    case SID_CLKH: { int h = 0, m = 0; if (Hal::clockValid()) Hal::clockGet(h, m); snprintf(b, n, "%02d", h); break; }
    case SID_CLKM: { int h = 0, m = 0; if (Hal::clockValid()) Hal::clockGet(h, m); snprintf(b, n, "%02d", m); break; }
    case SID_BT:       strlcpy(b, S.btName[0] ? S.btName : (Player::scanMode() ? "pairing..." : "not paired"), n); break;
    case SID_INFO1:    snprintf(b, n, "%d", Library::count()); break;
    case SID_INFO2:    snprintf(b, n, "%u KB", (unsigned)(ESP.getFreeHeap() / 1024)); break;
    default: break;
  }
}

static void cycleStep(SId id, int dir) {
  auto wrap = [](int v, int n) { return (v % n + n) % n; };
  switch (id) {
    case SID_REPEAT:   Player::setRepeat((uint8_t)wrap(S.repeat + dir, 3)); break;
    case SID_TIMEOUT:  S.timeoutIdx = wrap(S.timeoutIdx + dir, 5); Cfg::saveSoon(); break;
    case SID_CPU:      S.cpuIdx = wrap(S.cpuIdx + dir, 3); Hal::applyPower(); Cfg::saveSoon(); break;
    case SID_POWEROFF: S.powerOffIdx = wrap(S.powerOffIdx + dir, 4); Cfg::saveSoon(); break;
    case SID_LOOK:     Th::setLook((uint8_t)wrap(Th::look() + dir, 4)); needFull = true; break;
    case SID_ACCENT:   Th::setAccent((uint8_t)wrap(Th::accent() + dir, 8)); needFull = true; break;
    case SID_CLKH: { int h = 12, m = 0; if (Hal::clockValid()) Hal::clockGet(h, m); Hal::clockSet(wrap(h + dir, 24), m); break; }
    case SID_CLKM: { int h = 12, m = 0; if (Hal::clockValid()) Hal::clockGet(h, m); Hal::clockSet(h, wrap(m + dir, 60)); break; }
    default: break;
  }
}

static void setRow(TFT_eSPI& g, int y, int idx, bool pressed) {
  const Item& it = ITEMS[idx];
  uint16_t bgc = (pressed && it.t != I_SECTION && it.t != I_INFO) ? C_SU2 : C_BG;
  g.fillRect(0, y, SCR_W, SET_ROWH, bgc);
  char b[40];
  switch (it.t) {
    case I_SECTION:
      txt(g, it.label, 12, y + 17, 1, 1, C_ACC, bgc);
      g.drawFastHLine(12, y + 29, SCR_W - 24, C_SUR);
      break;
    case I_TOGGLE: {
      txt(g, it.label, 12, y + 8, 2, 1, C_TXT, bgc);
      bool on = toggleVal(it.id);
      g.fillRoundRect(190, y + 8, 38, 18, 9, on ? C_ACC : C_SU2);
      g.fillCircle(on ? 219 : 199, y + 17, 7, on ? C_BG : C_DIM);
      break;
    }
    case I_CYCLE:
      txt(g, it.label, 12, y + 8, 2, 1, C_TXT, bgc);
      valueText(it.id, b, sizeof(b));
      txt(g, b, 228, y + 8, 2, 1, C_ACC, bgc, TR_DATUM);
      break;
    case I_COLOR:
      txt(g, it.label, 12, y + 8, 2, 1, C_TXT, bgc);
      g.fillRoundRect(196, y + 7, 32, 18, 4, C_ACC);
      break;
    case I_SLIDER: {
      txt(g, it.label, 12, y + 8, 2, 1, C_TXT, bgc);
      int mn, mx, v; sliderRange(it.id, mn, mx, v);
      int fw = (v - mn) * 80 / (mx - mn);
      g.fillRoundRect(148, y + 13, 80, 6, 3, C_SU2);
      g.fillRoundRect(148, y + 13, max(fw, 4), 6, 3, C_ACC);
      g.fillCircle(148 + fw, y + 16, 7, C_TXT);
      break;
    }
    case I_ACTION:
      txt(g, it.label, 12, y + 8, 2, 1, C_ACC, bgc);
      break;
    case I_INFO:
      txt(g, it.label, 12, y + 8, 2, 1, C_DIM, bgc);
      valueText(it.id, b, sizeof(b));
      { char c[40]; fitText(g, b, 2, 140, c, sizeof(c)); txt(g, c, 228, y + 8, 2, 1, C_TXT, bgc, TR_DATUM); }
      break;
  }
}

static void setHeader() {
  T.fillRect(0, 0, SCR_W, SET_HDR, C_BG);
  T.fillTriangle(16, 16, 24, 9, 24, 23, C_ACC);
  txt(T, "Settings", 38, 8, 2, 1, C_TXT, C_BG);
  T.drawFastHLine(0, SET_HDR - 1, SCR_W, C_SU2);
}

static void setFull() { T.fillScreen(C_BG); setHeader(); setv.dirty = true; }

static void doRescan() {
  toast("Scanning card...", 60000);
  Player::stop();
  delay(150);
  Library::rescan();
  nameCacheClear();
  lib.setCount(Library::count(), LIB_ROWH);
  lib.pos = 0; lib.vel = 0; lib.kinetic = false;
  npLastCur = -2;
  char b[32]; snprintf(b, sizeof(b), "%d songs found", Library::count());
  toast(b, 2200);
}

static void setActivate(int idx, int x) {
  const Item& it = ITEMS[idx];
  switch (it.t) {
    case I_TOGGLE:
      switch (it.id) {
        case SID_SHUFFLE: Player::setShuffle(!S.shuffle); break;
        case SID_AUTOB:   S.autoBright = !S.autoBright; Hal::applyBrightness(); Cfg::saveSoon(); break;
        case SID_LED:     S.statusLed = !S.statusLed; Hal::applyPower(); Cfg::saveSoon(); break;
        case SID_ANIM:    S.animations = !S.animations; Cfg::saveSoon(); break;
        default: break;
      }
      break;
    case I_CYCLE: case I_COLOR:
      cycleStep(it.id, x < SCR_W / 2 ? -1 : 1);
      break;
    case I_ACTION:
      switch (it.id) {
        case SID_SCROFF: Hal::screenOff(); break;
        case SID_SLEEP:  Hal::deepSleep(); break;
        case SID_PAIR:
          if (Player::scanMode()) go(SC_PICK);
          else openDlg(DLG_PAIR, "Pair headphones", "Restarts to search for", "Bluetooth headphones.");
          break;
        case SID_WIFI:
          openDlg(DLG_WIFI, "WiFi receiver", "Restarts into a WiFi hotspot", "to receive songs via Telegram.");
          break;
        case SID_RESCAN: doRescan(); break;
        case SID_RECAL:  openDlg(DLG_RECAL, "Touch calibration", "Restarts, then tap three", "targets on the screen.");
          break;
        default: break;
      }
      break;
    default: break;
  }
}

static void setSliderTo(int idx, int x) {
  const Item& it = ITEMS[idx];
  int mn, mx, v; sliderRange(it.id, mn, mx, v);
  int nv = mn + (constrain(x, 148, 228) - 148) * (mx - mn) / 80;
  if (it.id == SID_VOLLIM) {
    S.volLimit = nv; Player::setVolume(S.volume);
  } else {
    S.brightness = nv; Hal::applyBrightness();
  }
  Cfg::saveSoon();
}

static void setTick(uint32_t now) {
  stepKinetic(setv, now, SET_LIST_H);
  static uint32_t lastInfo = 0;
  if (now - lastInfo > 2000) { lastInfo = now; setv.dirty = true; }     // live heap / bt state
  if (setv.dirty && now - setv.lastDraw >= 18) {
    renderList(setv, SET_HDR, SET_LIST_H, setRow);
    setv.lastDraw = now;
  }
}

static void setPress(int x, int y) {
  setv.kinetic = false; setv.vel = 0;
  sliderRow = -1;
  if (y >= SET_HDR) {
    setv.pressed = setv.rowAt(y, SET_HDR);
    if (setv.pressed >= 0 && ITEMS[setv.pressed].t == I_SLIDER && x >= 132) {
      sliderRow = setv.pressed;
      setSliderTo(sliderRow, x);
    }
    setv.dirty = true;
  }
}

static void setMove(int x, int y) {
  if (sliderRow >= 0) { setSliderTo(sliderRow, x); setv.dirty = true; return; }
  int dy = y - G.y0;
  if (!G.moved && abs(dy) > TAP_SLOP) { G.moved = true; setv.pressed = -1; }
  static float startPos = 0;
  if (!G.moved) startPos = setv.pos;
  if (G.moved) { setv.pos = constrain(startPos - dy, 0.0f, (float)setv.maxPos); setv.dirty = true; }
}

static void setRelease(int x, int y, bool tap) {
  int pr = setv.pressed;
  setv.pressed = -1; setv.dirty = true;
  if (sliderRow >= 0) { sliderRow = -1; return; }
  if (tap) {
    if (y < SET_HDR && x < 80) { go(SC_NOW); return; }
    if (pr >= 0) setActivate(pr, x);
  } else if (fabsf(G.vy) > 150) {
    setv.vel = constrain(G.vy, -2500.0f, 2500.0f);
    setv.kinetic = true;
    setv.lastStep = millis();
  }
}

// ================================================================== PICKER
static ListView pick;
static const int PICK_Y = 84, PICK_H = 196, PICK_ROWH = 36;
static int pickShown = -1;

static void pickRow(TFT_eSPI& g, int y, int idx, bool pressed) {
  uint16_t bgc = pressed ? C_SU2 : C_BG;
  g.fillRect(0, y, SCR_W, PICK_ROWH, bgc);
  char c[44]; fitText(g, Player::scanName(idx), 2, 210, c, sizeof(c));
  txt(g, c, 14, y + 10, 2, 1, C_TXT, bgc);
  g.drawFastHLine(12, y + PICK_ROWH - 1, SCR_W - 24, C_SUR);
}

static void pickFull() {
  T.fillScreen(C_BG);
  txt(T, "Pair headphones", 14, 10, 2, 1, C_TXT, C_BG);
  txt(T, "Put them in pairing mode, then", 14, 40, 1, 1, C_DIM, C_BG);
  txt(T, "tap their name below.", 14, 52, 1, 1, C_DIM, C_BG);
  T.drawFastHLine(0, PICK_Y - 2, SCR_W, C_SU2);
  T.fillRoundRect(14, 288, 212, 26, 6, C_SU2);
  txt(T, "Cancel", SCR_W / 2, 301, 2, 1, C_TXT, C_SU2, MC_DATUM);
  pickShown = -1;
  pick.dirty = true;
}

static void pickTick(uint32_t now) {
  int n = Player::scanCount();
  if (n != pickShown) { pickShown = n; pick.setCount(n, PICK_ROWH); pick.dirty = true; }
  stepKinetic(pick, now, PICK_H);
  if (pick.dirty && now - pick.lastDraw >= 18) {
    if (n == 0) {
      T.fillRect(0, PICK_Y, SCR_W, PICK_H, C_BG);
      txt(T, "Searching...", SCR_W / 2, PICK_Y + 60, 2, 1, C_DIM, C_BG, MC_DATUM);
      pick.dirty = false;
    } else renderList(pick, PICK_Y, PICK_H, pickRow);
    pick.lastDraw = now;
  }
}

static void pickPress(int x, int y) {
  pick.kinetic = false; pick.vel = 0;
  if (y >= PICK_Y && y < PICK_Y + PICK_H) { pick.pressed = pick.rowAt(y, PICK_Y); pick.dirty = true; }
}

static void pickMove(int x, int y) {
  int dy = y - G.y0;
  if (!G.moved && abs(dy) > TAP_SLOP) { G.moved = true; pick.pressed = -1; }
  static float startPos = 0;
  if (!G.moved) startPos = pick.pos;
  if (G.moved) { pick.pos = constrain(startPos - dy, 0.0f, (float)pick.maxPos); pick.dirty = true; }
}

static void pickRelease(int x, int y, bool tap) {
  int pr = pick.pressed;
  pick.pressed = -1; pick.dirty = true;
  if (!tap) return;
  if (y >= 284) {                                         // Cancel
    if (S.pairNext) { S.pairNext = 0; Cfg::save(); ESP.restart(); }
    go(SC_NOW);
    return;
  }
  if (pr >= 0) {
    strlcpy(S.btName, Player::scanName(pr), sizeof(S.btName));
    S.pairNext = 0;
    Cfg::save();
    T.fillScreen(C_BG);
    txt(T, "Connecting...", SCR_W / 2, SCR_H / 2, 2, 1, C_TXT, C_BG, MC_DATUM);
    delay(600);
    ESP.restart();
  }
}

// ================================================================== navigation + dispatch
static void go(Screen s) {
  if (s == scr) return;
  bool wasList = (scr == SC_LIB || scr == SC_SET || scr == SC_PICK);
  bool toList = (s == SC_LIB || s == SC_SET || s == SC_PICK);
  scr = s;
  needFull = true;
  if (wasList && !toList) sprRelease();                   // give the 19 KB back while playing
  if (s == SC_LIB) {
    lib.setCount(Library::count(), LIB_ROWH);
    lib.layout(LIB_LIST_H);
    int cur = Player::current();
    if (cur >= 0) lib.pos = constrain((float)(cur * LIB_ROWH - LIB_LIST_H / 2 + LIB_ROWH / 2), 0.0f, (float)lib.maxPos);
    lib.vel = 0; lib.kinetic = false; lib.pressed = -1;
  } else if (s == SC_SET) {
    setv.setCount(N_ITEMS, SET_ROWH);
    setv.layout(SET_LIST_H);
    setv.vel = 0; setv.kinetic = false; setv.pressed = -1;
  } else if (s == SC_PICK) {
    pick.setCount(Player::scanCount(), PICK_ROWH);
    pick.pos = 0; pick.vel = 0; pick.kinetic = false; pick.pressed = -1;
  } else {
    npLastCur = -2;                                       // force a meta refresh
  }
}

static void dispatchPress(int x, int y) {
  switch (scr) { case SC_NOW: npPress(x, y); break; case SC_LIB: libPress(x, y); break;
                 case SC_SET: setPress(x, y); break; case SC_PICK: pickPress(x, y); break; }
}
static void dispatchMove(int x, int y) {
  switch (scr) { case SC_NOW: npMove(x, y); break; case SC_LIB: libMove(x, y); break;
                 case SC_SET: setMove(x, y); break; case SC_PICK: pickMove(x, y); break; }
}
static void dispatchRelease(int x, int y, bool tap) {
  switch (scr) { case SC_NOW: npRelease(x, y, tap); break; case SC_LIB: libRelease(x, y, tap); break;
                 case SC_SET: setRelease(x, y, tap); break; case SC_PICK: pickRelease(x, y, tap); break; }
}

static void dlgTouch(int x, int y) {                      // modal: only its buttons react
  if (inRect(x, y, 28, 186, 88, 30)) { dlg.open = false; needFull = true; }
  else if (inRect(x, y, 124, 186, 88, 30)) { dlg.open = false; runDlg(); needFull = true; }
}

static void gestureStep(bool p, int16_t x, int16_t y, uint32_t now) {
  if (p) {
    G.miss = 0;
    if (!G.down) {
      G.down = true; G.moved = false; G.vy = 0;
      G.x0 = G.x = x; G.y0 = G.y = y; G.t0 = G.tl = now;
      if (dlg.open) return;
      dispatchPress(x, y);
    } else {
      int sx = (G.x + x) / 2, sy = (G.y + y) / 2;           // light smoothing for the resistive panel
      float dt = (now - G.tl) / 1000.0f;
      if (dt > 0.004f) { G.vy = 0.65f * G.vy + 0.35f * ((sy - G.y) / dt); G.tl = now; }
      G.x = sx; G.y = sy;
      if (!dlg.open) dispatchMove(sx, sy);
    }
  } else if (G.down) {
    if (++G.miss < 2) return;                               // debounce a dropped sample
    G.down = false;
    if (dlg.open) { dlgTouch(G.x, G.y); return; }
    bool tap = !G.moved && abs(G.x - G.x0) < TAP_SLOP + 4 && abs(G.y - G.y0) < TAP_SLOP + 4;
    if (G.moved && scr != SC_NOW && now - G.tl > 120) G.vy = 0;   // stopped before lifting
    dispatchRelease(G.x, G.y, tap);
  }
}

// ================================================================== public API
void UI::begin() {
  nameCacheClear();
  lib.setCount(Library::count(), LIB_ROWH);
  setv.setCount(N_ITEMS, SET_ROWH);
  scr = Player::scanMode() ? SC_PICK : SC_NOW;
  if (scr == SC_PICK) pick.setCount(Player::scanCount(), PICK_ROWH);
  needFull = true;
  active = true;
}

void UI::setActive(bool on) {
  active = on;
  if (on) needFull = true;
}

int UI::idleDelayMs() {
  if (G.down || lib.kinetic || setv.kinetic || pick.kinetic) return 3;
  return 8;
}

void UI::update() {
  uint32_t now = millis();
  int16_t x = 0, y = 0;
  bool p = Hal::readTouch(x, y);                            // also wakes a dark screen
  if (!active) return;

  if (needFull) {
    needFull = false;
    switch (scr) {
      case SC_NOW:  npFetchMeta(); npLastCur = Player::current(); npLastSerial = Player::serial(); npFull(); break;
      case SC_LIB:  libFull(); break;
      case SC_SET:  setFull(); break;
      case SC_PICK: pickFull(); break;
    }
    if (dlg.open) { drawDlg(); dlgDrawPending = false; }
    if (toastShown) drawToast();
  }

  gestureStep(p, x, y, now);

  if (dlg.open) {                                           // modal: nothing else repaints
    if (dlgDrawPending) { dlgDrawPending = false; drawDlg(); }
    return;
  }

  switch (scr) {
    case SC_NOW:  npTick(now); break;
    case SC_LIB:  libTick(now); break;
    case SC_SET:  setTick(now); break;
    case SC_PICK: pickTick(now); break;
  }

  if (toastShown && now > toastUntil) { toastShown = false; needFull = true; }
}
