#include "library.h"
#include "config.h"
#include <SD.h>
#include <SPI.h>
#include <algorithm>

static SPIClass sdSpi(VSPI);
static bool sdReady = false;

static char* pool = nullptr;        // paths (relative to the scan root), NUL separated
static uint16_t* offs = nullptr;    // offsets into pool, sorted by file name
static const size_t POOL_MAX = 65000;   // keeps uint16 offsets valid
static const char* rootPrefix = ""; // "" for a root scan, "/music" for MUSIC_DIR
static size_t rootSkip = 1;         // bytes of f.path() to skip to get the rel path
static int nTracks = 0;

static int passCount;
static size_t passBytes, fillPos;
static int fillN;
static bool filling;

static bool isMp3(const char* n) {
  size_t l = strlen(n);
  return l > 4 && strcasecmp(n + l - 4, ".mp3") == 0;
}

static const char* baseName(const char* p) {
  const char* s = strrchr(p, '/');
  return s ? s + 1 : p;
}

static void scanDir(const char* path, int depth) {
  File d = SD.open(path);
  if (!d) return;
  if (!d.isDirectory()) { d.close(); return; }
  for (;;) {
    File f = d.openNextFile();
    if (!f) break;
    const char* nm = f.name();
    if (nm[0] != '.') {
      if (f.isDirectory()) {
        if (depth > 0) {
          String sub = f.path();
          f.close();
          scanDir(sub.c_str(), depth - 1);
          continue;
        }
      } else if (isMp3(nm)) {
        const char* p = f.path() + rootSkip;   // store path relative to the scan root
        size_t L = strlen(p) + 1;
        if (L <= PATH_MAX_LEN && passCount < MAX_TRACKS &&
            passBytes + L <= POOL_MAX) {
          if (filling) {
            memcpy(pool + fillPos, p, L);
            offs[fillN++] = fillPos;
            fillPos += L;
          }
          passCount++;
          passBytes += L;
        }
      }
    }
    f.close();
  }
  d.close();
}

static void scanAll() {
  if (SD.exists(MUSIC_DIR)) {
    rootPrefix = MUSIC_DIR;
    rootSkip = strlen(MUSIC_DIR) + 1;      // "/music/" -> skip it too
    scanDir(MUSIC_DIR, 3);
  } else {
    rootPrefix = "";
    rootSkip = 1;                          // "/" -> skip the leading slash
    scanDir("/", 1);
  }
}

void Library::rescan() {
  free(pool); free(offs);
  pool = nullptr; offs = nullptr; nTracks = 0;
  if (!sdReady) return;
  passCount = 0; passBytes = 0; filling = false;
  scanAll();
  if (passCount == 0) return;
  pool = (char*)malloc(passBytes);
  offs = (uint16_t*)malloc(passCount * sizeof(uint16_t));
  if (!pool || !offs) { free(pool); free(offs); pool = nullptr; offs = nullptr; return; }
  filling = true; fillPos = 0; fillN = 0; passCount = 0; passBytes = 0;
  scanAll();
  nTracks = fillN;
  std::sort(offs, offs + nTracks, [](uint16_t a, uint16_t b) {
    return strcasecmp(baseName(pool + a), baseName(pool + b)) < 0;
  });
  Serial.printf("Library: %d tracks\n", nTracks);
}

void Library::begin() {
  sdSpi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sdReady = SD.begin(SD_CS, sdSpi, 20000000);
  Serial.printf("SD: %s\n", sdReady ? "ok" : "not found");
  rescan();
}

bool Library::sdOk() { return sdReady; }
int Library::count() { return nTracks; }
const char* Library::path(int i) {
  if (i < 0 || i >= nTracks) return "";
  static char buf[PATH_MAX_LEN];           // pool stores paths relative to root
  snprintf(buf, PATH_MAX_LEN, "%s/%s", rootPrefix, pool + offs[i]);
  return buf;
}

static void asciiCopy(const char* s, size_t len, char* out, size_t n) {
  size_t o = 0;
  for (size_t i = 0; i < len && o + 1 < n; i++) {
    uint8_t c = s[i];
    if (c < 0x80) out[o++] = c < 32 ? ' ' : c;
    else if (c >= 0xC0) out[o++] = '?';     // lead byte of a multi-byte char
  }
  out[o] = 0;
}

void Library::displayName(int i, char* title, size_t tn, char* artist, size_t an) {
  title[0] = 0; artist[0] = 0;
  if (i < 0 || i >= nTracks) return;
  const char* b = baseName(pool + offs[i]);
  size_t len = strlen(b);
  if (len > 4) len -= 4;                       // strip .mp3
  char tmp[96];
  asciiCopy(b, len, tmp, sizeof(tmp));
  for (char* p = tmp; *p; p++) if (*p == '_') *p = ' ';
  char* sep = strstr(tmp, " - ");
  if (sep) {
    *sep = 0;
    strlcpy(artist, tmp, an);
    strlcpy(title, sep + 3, tn);
  } else {
    strlcpy(title, tmp, tn);
  }
}
