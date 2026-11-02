// Audio engine:   SD file -> Helix MP3 decoder (core 0 task) -> ring buffer -> A2DP callback (BT task)
#include "player.h"
#include "config.h"
#include "settings.h"
#include "library.h"
#include <SD.h>
#include <esp_bt.h>
#include <BluetoothA2DPSource.h>
#include <MP3DecoderHelix.h>

using namespace libhelix;

// ---------------------------------------------------------------- ring buffer
struct Ring {
  uint8_t* b = nullptr;
  size_t sz = 0, r = 0, w = 0, n = 0;
  SemaphoreHandle_t m = nullptr;

  bool init(size_t s) {
    b = (uint8_t*)malloc(s);
    sz = s;
    m = xSemaphoreCreateMutex();
    return b && m;
  }
  size_t count() {
    xSemaphoreTake(m, portMAX_DELAY);
    size_t c = n;
    xSemaphoreGive(m);
    return c;
  }
  size_t write(const uint8_t* d, size_t len) {            // partial write, never blocks long
    xSemaphoreTake(m, portMAX_DELAY);
    size_t k = min(len, sz - n);
    size_t first = min(k, sz - w);
    memcpy(b + w, d, first);
    memcpy(b, d + first, k - first);
    w = (w + k) % sz;
    n += k;
    xSemaphoreGive(m);
    return k;
  }
  size_t read(uint8_t* d, size_t len) {
    if (xSemaphoreTake(m, pdMS_TO_TICKS(3)) != pdTRUE) return 0;
    size_t k = min(len, n);
    size_t first = min(k, sz - r);
    memcpy(d, b + r, first);
    memcpy(d + first, b, k - first);
    r = (r + k) % sz;
    n -= k;
    xSemaphoreGive(m);
    return k;
  }
  void flush() {
    xSemaphoreTake(m, portMAX_DELAY);
    r = w = n = 0;
    xSemaphoreGive(m);
  }
};

static Ring ring;

// ---------------------------------------------------------------- shared state
enum CmdType : uint8_t { CMD_OPEN, CMD_SEEK, CMD_STOP };
struct Cmd { uint8_t type; float frac; char path[PATH_MAX_LEN]; };

static QueueHandle_t cmdQ;
static MP3DecoderHelix* mp3 = nullptr;
static BluetoothA2DPSource a2dp;

static volatile bool g_paused = true;      // user pause (or nothing playing)
static volatile bool g_loaded = false;     // a track is open
static volatile bool g_switching = false;  // open/seek in flight -> output silence
static volatile bool g_eof = false;        // decoder read the whole file
static volatile bool g_fail = false;       // could not open file
static volatile uint32_t g_gen = 0;        // bumped on every command (aborts blocking writes)
static volatile uint32_t g_serial = 0;
static volatile uint32_t g_frames = 0;     // PCM frames delivered to Bluetooth since open/seek
static volatile uint32_t g_dur = 0;
static volatile float g_base = 0;          // seconds at open/seek point
static volatile uint16_t g_gainQ8 = 100;
static uint32_t decGen = 0;
static uint32_t g_aStart = 0, g_aEnd = 0;
static int g_cur = -1;
static char g_title[64], g_artist[48];
static int g_hist[16];
static int g_histN = 0;

static bool g_scan = false;
static char g_scanNames[8][40];
static volatile int g_scanN = 0;

// ---------------------------------------------------------------- PCM path (decoder side)
static float rsPos = 0, rsPrevL = 0, rsPrevR = 0;
static void resetResampler() { rsPos = 0; rsPrevL = rsPrevR = 0; }

static void pushPcm(const uint8_t* d, size_t len) {
  while (len) {
    if (g_gen != decGen) return;                // new command pending: drop this data
    size_t k = ring.write(d, len);
    d += k; len -= k;
    if (len) vTaskDelay(pdMS_TO_TICKS(5));
  }
}

static void pcmCb(MP3FrameInfo& info, short* pcm, size_t len, void*) {
  if (!g_dur && info.bitrate > 0 && g_aEnd > g_aStart)
    g_dur = (uint32_t)(((uint64_t)(g_aEnd - g_aStart) * 8) / info.bitrate);
  int ch = info.nChans < 1 ? 1 : info.nChans;
  size_t frames = len / ch;
  uint32_t rate = info.samprate;

  if (ch == 2 && rate == 44100) {               // fast path
    pushPcm((const uint8_t*)pcm, frames * 4);
    return;
  }
  // mono and/or other sample rates: duplicate channels + linear resample to 44.1 kHz
  static int16_t out[512];
  size_t o = 0;
  float step = rate / 44100.0f;
  for (size_t i = 0; i < frames; i++) {
    float L = pcm[i * ch];
    float R = ch >= 2 ? pcm[i * ch + 1] : L;
    while (rsPos < 1.0f) {
      out[o++] = (int16_t)(rsPrevL + (L - rsPrevL) * rsPos);
      out[o++] = (int16_t)(rsPrevR + (R - rsPrevR) * rsPos);
      rsPos += step;
      if (o >= 512) { pushPcm((const uint8_t*)out, o * 2); o = 0; }
    }
    rsPos -= 1.0f;
    rsPrevL = L; rsPrevR = R;
  }
  if (o) pushPcm((const uint8_t*)out, o * 2);
}

// ---------------------------------------------------------------- ID3v2 (title/artist) + audio start
static void id3Text(const uint8_t* d, size_t n, char* out, size_t on) {
  out[0] = 0;
  if (n < 2) return;
  uint8_t enc = d[0]; d++; n--;
  size_t o = 0;
  if (enc == 0 || enc == 3) {                   // Latin-1 / UTF-8
    for (size_t i = 0; i < n && o + 1 < on; i++) {
      uint8_t c = d[i];
      if (c == 0) break;
      if (c < 0x80) out[o++] = c >= 32 ? c : ' ';
      else if (c >= 0xC0) out[o++] = '?';
    }
  } else {                                      // UTF-16
    bool be = (enc == 2);
    size_t i = 0;
    if (enc == 1 && n >= 2) {
      if (d[0] == 0xFF && d[1] == 0xFE) { be = false; i = 2; }
      else if (d[0] == 0xFE && d[1] == 0xFF) { be = true; i = 2; }
    }
    for (; i + 1 < n && o + 1 < on; i += 2) {
      uint16_t ch = be ? ((d[i] << 8) | d[i + 1]) : ((d[i + 1] << 8) | d[i]);
      if (ch == 0) break;
      out[o++] = ch < 0x80 ? (ch >= 32 ? (char)ch : ' ') : '?';
    }
  }
  out[o] = 0;
}

static uint32_t id3Parse(File& f) {
  uint8_t h[10];
  f.seek(0);
  if (f.read(h, 10) != 10 || memcmp(h, "ID3", 3) != 0) return 0;
  uint8_t ver = h[3];
  uint32_t size = ((h[6] & 0x7F) << 21) | ((h[7] & 0x7F) << 14) | ((h[8] & 0x7F) << 7) | (h[9] & 0x7F);
  uint32_t end = 10 + size;
  bool ext = h[5] & 0x40;
  if ((ver == 3 || ver == 4) && !ext) {
    char t[64] = {0}, a[48] = {0};
    uint32_t pos = 10;
    while (pos + 10 <= end) {
      uint8_t fh[10];
      f.seek(pos);
      if (f.read(fh, 10) != 10 || fh[0] == 0) break;
      uint32_t fs = ver == 4
        ? ((fh[4] & 0x7F) << 21) | ((fh[5] & 0x7F) << 14) | ((fh[6] & 0x7F) << 7) | (fh[7] & 0x7F)
        : ((uint32_t)fh[4] << 24) | ((uint32_t)fh[5] << 16) | ((uint32_t)fh[6] << 8) | fh[7];
      if (fs == 0 || pos + 10 + fs > end) break;
      bool isT = !memcmp(fh, "TIT2", 4), isA = !memcmp(fh, "TPE1", 4);
      if ((isT || isA) && fs <= 400) {
        uint8_t tmp[400];
        size_t n = f.read(tmp, fs);
        if (isT) id3Text(tmp, n, t, sizeof(t)); else id3Text(tmp, n, a, sizeof(a));
      }
      pos += 10 + fs;
    }
    if (t[0]) strlcpy(g_title, t, sizeof(g_title));
    if (a[0]) strlcpy(g_artist, a, sizeof(g_artist));
  }
  return end;
}

// ---------------------------------------------------------------- decoder task
static void doSeek(File& f, float frac) {
  uint32_t pos = g_aStart + (uint32_t)(frac * (float)(g_aEnd - g_aStart));
  f.seek(pos);
  mp3->end(); mp3->begin();
  resetResampler();
  ring.flush();
  g_base = g_dur ? frac * g_dur : 0;
  g_frames = 0;
  g_eof = false;
}

static void decodeTask(void*) {
  File f;
  bool open = false;
  static uint8_t buf[1024];
  for (;;) {
    Cmd c;
    bool got = xQueueReceive(cmdQ, &c, (open && !g_eof) ? 0 : pdMS_TO_TICKS(30)) == pdTRUE;
    if (got) {
      decGen = g_gen;
      if (c.type == CMD_STOP) {
        if (f) f.close();
        mp3->end();
        ring.flush();
        open = false; g_loaded = false; g_eof = false; g_switching = false;
        continue;
      }
      if (c.type == CMD_OPEN) {
        if (f) f.close();
        mp3->end();
        ring.flush();
        resetResampler();
        g_frames = 0; g_base = 0; g_dur = 0; g_eof = false; g_fail = false;
        f = SD.open(c.path, FILE_READ);
        if (!f) {
          g_fail = true; g_eof = true; open = false; g_loaded = true; g_switching = false;
          g_serial++;
          continue;
        }
        g_aStart = id3Parse(f);
        g_aEnd = f.size();
        if (g_aStart >= g_aEnd) g_aStart = 0;
        f.seek(g_aStart);
        mp3->begin();
        open = true; g_loaded = true; g_switching = false;
        g_serial++;
        continue;
      }
      if (c.type == CMD_SEEK && open) {
        doSeek(f, c.frac);
        g_switching = false;
        continue;
      }
      g_switching = false;
      continue;
    }
    if (!open || g_eof) continue;

    int n = f.read(buf, sizeof(buf));
    if (n <= 0) { g_eof = true; continue; }
    size_t off = 0;
    while (off < (size_t)n) {
      if (g_gen != decGen) break;
      size_t w = mp3->write(buf + off, n - off);
      if (!w) break;
      off += w;
    }
  }
}

// ---------------------------------------------------------------- Bluetooth side
static int32_t a2dpData(Frame* out, int32_t frames) {
  size_t need = (size_t)frames * 4;
  if (g_paused || !g_loaded || g_switching) {
    memset(out, 0, need);                        // keep the link alive with silence
    return frames;
  }
  size_t got = ring.read((uint8_t*)out, need);
  int16_t* s = (int16_t*)out;
  size_t cnt = got / 2;
  uint32_t g = g_gainQ8;
  for (size_t i = 0; i < cnt; i++) s[i] = (int16_t)(((int32_t)s[i] * (int32_t)g) >> 8);
  if (got < need) memset((uint8_t*)out + got, 0, need - got);
  g_frames += got / 4;
  return frames;
}

static bool onFound(const char* ssid, esp_bd_addr_t, int) {
  if (!g_scan || !ssid || !ssid[0]) return false;
  for (int i = 0; i < g_scanN; i++) if (!strcmp(g_scanNames[i], ssid)) return false;
  if (g_scanN < 8) {
    strlcpy(g_scanNames[g_scanN], ssid, sizeof(g_scanNames[0]));
    g_scanN++;
  }
  return false;                                  // never auto-connect; the user picks
}

// ---------------------------------------------------------------- API
namespace Player {

static void applyVolume() {
  uint8_t v = min(S.volume, S.volLimit);
  float f = v / 100.0f;
  g_gainQ8 = (uint16_t)(f * f * 256.0f);
}

static void post(const Cmd& c) {
  g_gen++;
  xQueueOverwrite(cmdQ, &c);
}

static void startTrack(int idx, bool pushHist) {
  if (idx < 0 || idx >= Library::count()) return;
  if (pushHist && g_cur >= 0 && g_cur != idx) {
    if (g_histN == 16) { memmove(g_hist, g_hist + 1, 15 * sizeof(int)); g_histN = 15; }
    g_hist[g_histN++] = g_cur;
  }
  g_cur = idx;
  Library::displayName(idx, g_title, sizeof(g_title), g_artist, sizeof(g_artist));
  Cmd c;
  c.type = CMD_OPEN; c.frac = 0;
  strlcpy(c.path, Library::path(idx), sizeof(c.path));
  g_switching = true;
  g_paused = false;
  post(c);
}

static int randomOther() {
  int n = Library::count();
  if (n < 2) return 0;
  int r;
  do { r = esp_random() % n; } while (r == g_cur);
  return r;
}

void begin() {
  if (!ring.init(RING_BYTES)) Serial.println("PCM ring alloc failed!");
  cmdQ = xQueueCreate(1, sizeof(Cmd));
  mp3 = new MP3DecoderHelix(pcmCb);
  applyVolume();
  xTaskCreatePinnedToCore(decodeTask, "mp3dec", 6144, nullptr, 3, nullptr, 0);

  // this app only uses classic BT (A2DP); release the BLE pools (~10 KB)
  // that the BT connect below would otherwise starve on
  esp_bt_mem_release(ESP_BT_MODE_BLE);
  Serial.printf("[heap] before a2dp: %u\n", (unsigned)ESP.getFreeHeap());

  g_scan = S.pairNext || !S.btName[0];
  a2dp.set_auto_reconnect(true);
  a2dp.set_data_callback_in_frames(a2dpData);

  // heap gate: the A2DP connect crashes the stack when system RAM is nearly
  // empty (receiver mode's WiFi/TLS statics live in the same image). Wait a
  // little for SD scanning to settle, then go anyway if it never rises.
  int h = (int)ESP.getFreeHeap();
  for (int tries = 0; h < 24000 && tries < 60; tries++) {
    vTaskDelay(pdMS_TO_TICKS(250));
    h = (int)ESP.getFreeHeap();
  }
  Serial.printf("[heap] a2dp.start with %u\n", (unsigned)h);

  if (g_scan) {
    a2dp.set_ssid_callback(onFound);
    a2dp.start();
  } else {
    a2dp.start(S.btName);
  }
}

void play(int idx) { startTrack(idx, true); }

void toggle() {
  if (!g_loaded) startTrack(g_cur >= 0 ? g_cur : 0, false);
  else g_paused = !g_paused;
}

void next() {
  int n = Library::count();
  if (!n) return;
  int i = (S.shuffle && n > 1) ? randomOther() : (g_cur + 1) % n;
  startTrack(i, true);
}

void prev() {
  int n = Library::count();
  if (!n) return;
  if (g_loaded && position() > 3.0f) { seek(0); return; }
  if (g_histN > 0) { startTrack(g_hist[--g_histN], false); return; }
  int i = (S.shuffle && n > 1) ? randomOther() : (g_cur - 1 + n) % n;
  startTrack(i, false);
}

void stop() {
  g_paused = true;
  Cmd c; c.type = CMD_STOP; c.frac = 0; c.path[0] = 0;
  g_switching = false;
  g_loaded = false;
  g_base = 0; g_frames = 0;
  post(c);
}

void seek(float frac) {
  if (!g_loaded || g_switching) return;
  Cmd c; c.type = CMD_SEEK; c.frac = constrain(frac, 0.0f, 0.999f); c.path[0] = 0;
  g_switching = true;
  post(c);
}

void update() {
  static uint8_t fails = 0;
  if (!g_loaded || g_switching) return;
  if (g_fail) {                                  // unreadable file: skip, but don't loop forever
    g_fail = false;
    if (++fails > 5) { fails = 0; stop(); return; }
    next();
    return;
  }
  if (g_eof && !g_paused && ring.count() == 0) {  // track finished and buffer drained
    fails = 0;
    int n = Library::count();
    if (!n) { stop(); return; }
    if (S.repeat == 2) { startTrack(g_cur, false); return; }
    if (S.shuffle && n > 1) { startTrack(randomOther(), true); return; }
    if (g_cur + 1 < n) { startTrack(g_cur + 1, true); return; }
    if (S.repeat == 1) { startTrack(0, true); return; }
    stop();
  }
}

bool active() { return g_loaded && !g_paused; }
bool paused() { return g_paused; }
bool loaded() { return g_loaded; }
int current() { return g_cur; }
uint32_t serial() { return g_serial; }
float position() { return g_base + g_frames / 44100.0f; }
uint32_t duration() { return g_dur; }
float fraction() {
  if (!g_dur) return 0;
  return constrain(position() / (float)g_dur, 0.0f, 1.0f);
}

void setVolume(uint8_t v) {
  S.volume = min(v, S.volLimit);
  applyVolume();
  Cfg::saveSoon();
}
uint8_t volume() { return S.volume; }
void setShuffle(bool on) { S.shuffle = on; Cfg::saveSoon(); }
void setRepeat(uint8_t m) { S.repeat = m; Cfg::saveSoon(); }

void meta(char* title, size_t tn, char* artist, size_t an) {
  strlcpy(title, g_title, tn);
  strlcpy(artist, g_artist, an);
}

bool btConnected() { return a2dp.is_connected(); }
bool scanMode() { return g_scan; }
int scanCount() { return g_scanN; }
const char* scanName(int i) { return (i >= 0 && i < g_scanN) ? g_scanNames[i] : ""; }

}  // namespace Player
