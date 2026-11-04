#include "library.h"
#include "config.h"
#include "settings.h"
#include <SD.h>
#include <SPI.h>
#include <algorithm>

static SPIClass sdSpi(VSPI);
static bool sdReady = false;

// ---------------------------------------------------------------- storage
// The song table lives on the SD card as /music.idx. RAM keeps only a small
// array of byte offsets into that file (800 tracks = 3.2 KB instead of a full
// copy of every path) — that heap is exactly what LVGL and Bluetooth fight
// over when the card is inserted.
//
//   /music.idx layout (little endian):
//     u32  magic 'MPDI'            // header
//     u16  version (IDX_VER)
//     u16  count N
//     u16  root flag (1 = paths relative to /music, 0 = relative to /)
//     uint32 off[N]                // byte offset of each path string
//     ...                          // NUL-terminated relative paths, sorted by
//                                  // base name ("file name" order)
//
// It is rewritten only when the library actually changes (Rescan button or
// the receiver writing new files sets a dirty flag), so normal boots skip
// the two-pass scan completely.

static const char* IDX_PATH = "/music.idx";
#define IDX_MAGIC 0x4944504DUL     // "MPDI"
#define IDX_VER   2

static File idxFile;               // stays open, used for on-demand lookups
static uint32_t* offs = nullptr;   // byte offsets into idxFile
static int nTracks = 0;
static bool idxOk = false;
static bool rootIsMusic = false;

// scanning temporaries — only live while building the index, freed after
static char* pool = nullptr;               // relative paths, NUL separated
static uint16_t* tofs = nullptr;           // temp offsets into pool
static const size_t POOL_MAX = 65000;      // keeps uint16 offsets valid
static const char* rootPrefix = "";        // "" for a root scan, "/music" for MUSIC_DIR
static size_t rootSkip = 1;                // bytes of f.path() to skip
static int passCount;
static size_t passBytes, fillPos;
static int fillN;
static bool filling;

// FATFS is not guaranteed reentrant, and the decoder task streams the card
// from core 0 while the UI task reads the index from core 1. One mutex for
// every file operation that crosses tasks.
static SemaphoreHandle_t sdMx = nullptr;
static bool libStarted = false;

void Library::sdEnter() { if (sdMx) xSemaphoreTake(sdMx, portMAX_DELAY); }
void Library::sdExit()  { if (sdMx) xSemaphoreGive(sdMx); }

// ------------------------------------------------------------ scan helpers

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
          if (sub != MUSIC_DIR) scanDir(sub.c_str(), depth - 1);
          continue;
        }
      } else if (isMp3(nm)) {
        const char* p = f.path() + rootSkip;   // store path relative to the scan root
        size_t L = strlen(p) + 1;
        if (L <= PATH_MAX_LEN && passCount < MAX_TRACKS &&
            passBytes + L <= POOL_MAX) {
          if (filling) {
            memcpy(pool + fillPos, p, L);
            tofs[fillN++] = fillPos;
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

static void scanAll() {  if (SD.exists(MUSIC_DIR)) {
    rootIsMusic = true;
    rootPrefix = MUSIC_DIR;
    rootSkip = strlen(MUSIC_DIR) + 1;      // "/music/" -> skip it too
    scanDir(MUSIC_DIR, 3);
    if (passCount > 0) return;             // music lives in /music
    // empty /music -> fall back to the whole card (root + folders, 3 deep)
    passCount = 0; passBytes = 0; fillPos = 0; fillN = 0;
  }
  rootIsMusic = false;
  rootPrefix = "";
  rootSkip = 1;                            // "/" -> skip the leading slash
  scanDir("/", 3);                         // MUSIC_DIR subtree is skipped below
}

// ------------------------------------------------------------ index file

static void closeIdx() {
  if (idxFile) idxFile.close();
  idxFile = File();
  free(offs);
  offs = nullptr;
  nTracks = 0;
  idxOk = false;
}

static bool loadIdx() {
  closeIdx();
  File f = SD.open(IDX_PATH, FILE_READ);
  if (!f) return false;
  uint32_t magic = 0;
  uint16_t ver = 0, cnt = 0, rflag = 0;
  bool ok = f.read((uint8_t*)&magic, 4) == 4 && magic == IDX_MAGIC &&
            f.read((uint8_t*)&ver, 2) == 2 && ver == IDX_VER &&
            f.read((uint8_t*)&cnt, 2) == 2 && cnt <= MAX_TRACKS &&
            f.read((uint8_t*)&rflag, 2) == 2;
  size_t fsz = f.size();
  if (ok && fsz < 10 + 4ul * cnt) ok = false;
  if (ok) {
    offs = (uint32_t*)malloc((cnt ? cnt : 1) * sizeof(uint32_t));
    ok = offs && f.read((uint8_t*)offs, 4 * cnt) == 4 * cnt;
  }
  if (ok) {
    for (int i = 0; i < cnt; i++) {          // offsets must climb, stay in-file
      if (offs[i] < 10 + 4ul * cnt || offs[i] >= fsz ||
          (i && offs[i] <= offs[i - 1])) { ok = false; break; }
    }
  }
  if (ok && cnt) {                           // first entry must be a real string
    f.seek(offs[0]);
    char probe[8] = {0};
    ok = f.read((uint8_t*)probe, 4) == 4 && probe[0] != 0;
  }
  if (!ok) {
    Serial.println("Library: /music.idx invalid - rescan needed");
    f.close();
    free(offs); offs = nullptr;
    return false;
  }
  idxFile = f;
  nTracks = cnt;
  rootIsMusic = rflag != 0;
  idxOk = true;
  Serial.printf("Library: %d tracks (from index)\n", nTracks);
  return true;
}

// writes /music.idx from pool+tofs (freshly scanned, sorted) and frees them
static bool publishIdx() {
  int n = fillN;
  uint32_t pos = 10 + 4ul * n;
  uint32_t* fo = (uint32_t*)malloc((n ? n : 1) * sizeof(uint32_t));
  if (!fo) return false;
  for (int k = 0; k < n; k++) {              // string positions are deterministic
    fo[k] = pos;
    pos += strlen(pool + tofs[k]) + 1;
  }
  SD.remove(IDX_PATH);
  bool ok = false;
  File f = SD.open(IDX_PATH, "w");
  if (f) {
    // header + offset table FIRST, then the strings append right after
    // them — the offsets then match the physical layout exactly (seeking
    // back over already-written strings would corrupt the first entries)
    uint32_t magic = IDX_MAGIC;
    uint16_t ver = IDX_VER, cnt = (uint16_t)n, rf = rootIsMusic ? 1 : 0;
    ok = f.write((uint8_t*)&magic, 4) == 4 &&
         f.write((uint8_t*)&ver, 2) == 2 &&
         f.write((uint8_t*)&cnt, 2) == 2 &&
         f.write((uint8_t*)&rf, 2) == 2 &&
         f.write((uint8_t*)fo, 4 * n) == 4 * n;
    for (int k = 0; k < n && ok; k++) {
      const char* s = pool + tofs[k];
      ok = f.write((const uint8_t*)s, strlen(s) + 1) == strlen(s) + 1;
    }
    f.close();
  }
  free(fo);
  return ok;
}

// ------------------------------------------------------------ public API

bool Library::started() { return libStarted; }

bool Library::start() {                       // called lazily: first play / library view
  if (libStarted) return sdReady;
  // Bluetooth init owns the heap for its first seconds: the SD mount and the
  // index load allocate tens of KB and must not race bluedroid's startup.
  while (g_btIniting) vTaskDelay(pdMS_TO_TICKS(20));
  libStarted = true;
  sdMx = xSemaphoreCreateMutex();
  Serial.printf("[lib] pre-spi free=%u\n", (unsigned)ESP.getFreeHeap());
  uint32_t preSd = ESP.getFreeHeap();
  sdSpi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  Serial.printf("[lib] pre-sd free=%u\n", (unsigned)ESP.getFreeHeap());
  sdReady = SD.begin(SD_CS, sdSpi, 20000000);
  Serial.printf("[lib] SD=%d free=%u (mount cost %u)\n", (int)sdReady,
                (unsigned)ESP.getFreeHeap(),
                (unsigned)(preSd - ESP.getFreeHeap()));
  if (!sdReady) return false;
  bool rebuild = Cfg::libDirty();            // receiver downloaded new files
  if (!rebuild) rebuild = !loadIdx();        // no (or broken/stale-format) index
  if (!rebuild && nTracks == 0) {            // empty index but not dirty: new
    rebuild = true;                          // files may have been copied by PC
  }
  if (rebuild) {
    Cfg::setLibDirty(false);                 // clear first: never rescan-loop on failure
    Serial.println("Library: building /music.idx (scanning card) ...");
    rescan();                                // stop playback first (UI does that)
  }
  Serial.printf("[lib] ready: n=%d free=%u\n", (int)nTracks,
                (unsigned)ESP.getFreeHeap());
  return true;
}

// one-time diagnostic helper: prints the card layout when the scan finds no
// mp3 files, so the card's true structure is visible on the serial monitor
static int dumpCnt = 0;
static void dumpDir(const char* path, int depth) {
  File d = SD.open(path);
  if (!d) return;
  for (;;) {
    if (dumpCnt > 150) { d.close(); return; }
    File f = d.openNextFile();
    if (!f) break;
    Serial.printf("[card] %s %s\n", f.isDirectory() ? "DIR " : "FILE", f.path());
    dumpCnt++;
    if (f.isDirectory() && depth > 0 && f.name()[0] != '.') {
      String sub = f.path();
      f.close();
      dumpDir(sub.c_str(), depth - 1);
      continue;
    }
    f.close();
  }
  d.close();
}

void Library::rescan() {
  if (!sdReady) return;
  closeIdx();
  free(pool); pool = nullptr;
  free(tofs); tofs = nullptr;
  passCount = 0; passBytes = 0; filling = false;
  scanAll();                                 // pass 1: count and size up
  Serial.printf("[scan] pass1: %d tracks, %u bytes\n", passCount, (unsigned)passBytes);
  if (passCount == 0) {                      // no mp3 anywhere we looked:
    Serial.println("[card] listing of the card (no mp3 found yet):");
    dumpCnt = 0;                             // show where the files actually are
    dumpDir("/", 2);
  }
  if (passCount > 0) {
    pool = (char*)malloc(passBytes);
    tofs = (uint16_t*)malloc(passCount * sizeof(uint16_t));
    if (pool && tofs) {
      filling = true; fillPos = 0; fillN = 0; passCount = 0; passBytes = 0;
      scanAll();                             // pass 2: fill the tables
      bool ok = publishIdx();
      Serial.printf("[scan] pass2: %d tracks, index %s\n", fillN, ok ? "written" : "FAILED");
    } else {
      Serial.println("[scan] ran out of RAM for the scan tables");
    }
  }
  free(pool); pool = nullptr;
  free(tofs); tofs = nullptr;
  loadIdx();                                 // switch to the small-footprint view
}

bool Library::sdOk() { return sdReady; }
int Library::count() { return nTracks; }

// fills relBuf: the stored path, relative to the scan root
static char relBuf[PATH_MAX_LEN];
static bool fillRel(int i) {
  relBuf[0] = 0;
  if (!idxOk || i < 0 || i >= nTracks) return false;
  Library::sdEnter();
  bool ok = idxFile.seek(offs[i]);
  int n = ok ? (int)idxFile.read((uint8_t*)relBuf, sizeof(relBuf) - 1) : 0;
  Library::sdExit();
  if (n <= 0) return false;
  char* z = (char*)memchr(relBuf, 0, n);     // path strings carry their own NUL
  if (z) *z = 0;
  else relBuf[n] = 0;
  return relBuf[0] != 0;
}

static char pathBuf[PATH_MAX_LEN + 8];
const char* Library::path(int i) {
  if (!fillRel(i)) return "";
  snprintf(pathBuf, sizeof(pathBuf), "%s/%s", rootIsMusic ? MUSIC_DIR : "", relBuf);
  return pathBuf;
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
  if (!fillRel(i)) return;
  const char* b = baseName(relBuf);
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
