#include "art.h"
#include "config.h"
#include "library.h"
#include "arthash.h"
#include <SD.h>
#include <Preferences.h>

namespace Art {

static const int PX = 200;                       // cached cover: PX x PX, RGB565

struct Hdr {                                     // 16-byte header, written by the PC tool
  char     m[4];                                 // "MCOV"
  uint16_t w, h;                                 // PX x PX
  uint32_t srcSize;                              // size of the mp3 it belongs to (informational)
  uint32_t pad;
};

// Cover colour fix, cycled in Settings > Cover colours (stored in NVS). 0 normal, 1 red/blue swapped,
// 2 inverted, 3 both. Only needed if a particular panel shows the covers with wrong colours.
static int8_t s_mode = -1;
int colorMode() {
  if (s_mode < 0) {
    Preferences p;
    p.begin("mypod", true);
    s_mode = (int8_t)(p.getUChar("covfix", 0) & 3);
    p.end();
  }
  return s_mode;
}
void setColorMode(int m) {
  s_mode = (int8_t)(m & 3);
  Preferences p;
  p.begin("mypod", false);
  p.putUChar("covfix", (uint8_t)s_mode);
  p.end();
}

bool exists(const char* song) {
  char cp[40];
  artCachePath(song, cp, sizeof(cp));
  Library::sdEnter();
  bool e = SD.exists(cp);
  Library::sdExit();
  return e;
}

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }


bool drawCover(TFT_eSPI& g, const char* song, int x, int y, int clipX0, int clipX1) {
  char cp[40];
  artCachePath(song, cp, sizeof(cp));
  Hdr hd;
  Library::sdEnter();
  File f = SD.open(cp, FILE_READ);
  bool ok = f && f.read((uint8_t*)&hd, sizeof(hd)) == (int)sizeof(hd) &&
            !memcmp(hd.m, "MCOV", 4) && hd.w == PX && hd.h == PX;
  if (!ok && f) f.close();
  Library::sdExit();
  if (!ok) return false;

  int c0 = imax(0, clipX0 - x), c1 = imin(PX, clipX1 - x);
  if (c1 <= c0) {                                // fully outside the art box
    Library::sdEnter(); f.close(); Library::sdExit();
    return true;
  }
  int br = 4;                                    // rows per SD read / SPI burst (small: heap is tight)
  uint16_t* buf = (uint16_t*)malloc(PX * br * 2);
  if (!buf) { br = 1; buf = (uint16_t*)malloc(PX * 2); }
  if (!buf) { Library::sdEnter(); f.close(); Library::sdExit(); return false; }

  int mode = colorMode();
  bool oldSwap = g.getSwapBytes();
  g.setSwapBytes(true);                          // cache holds native RGB565 words
  bool complete = true;
  for (int r = 0; r < PX; r += br) {
    int rows = imin(br, PX - r);
    Library::sdEnter();                          // short hold: the decoder shares this lock
    f.seek(sizeof(Hdr) + (uint32_t)r * PX * 2);
    int n = f.read((uint8_t*)buf, rows * PX * 2);
    Library::sdExit();
    if (n != rows * PX * 2) { complete = false; break; }
    if (mode) {                                  // colour fix, see colorMode()
      for (int i = 0; i < rows * PX; i++) {
        uint16_t w = buf[i];
        if (mode & 1) w = (uint16_t)(((w & 0x001F) << 11) | (w & 0x07E0) | (w >> 11));
        if (mode & 2) w = (uint16_t)~w;
        buf[i] = w;
      }
    }
    if (c0 == 0 && c1 == PX) g.pushImage(x, y + r, PX, rows, buf);
    else for (int k = 0; k < rows; k++) g.pushImage(x + c0, y + r + k, c1 - c0, 1, buf + k * PX + c0);
  }
  g.setSwapBytes(oldSwap);
  free(buf);
  Library::sdEnter(); f.close(); Library::sdExit();
  return complete;
}


} // namespace Art
