// Runs the REAL device protocol engine on the PC. stdin/stdout play the USB serial port.
// usage: host_device <sd-root-dir>
#define USBSYNC_HOST 1
#include "../../src/usbsync_engine.h"
#ifdef _WIN32
#  define NOMINMAX
#  include <windows.h>        // PeekNamedPipe / QueryPerformanceCounter / Sleep
#  include <fcntl.h>          // _O_BINARY for _setmode
#  include <io.h>             // _get_osfhandle; MinGW CRT read/write/mkdir/_setmode
#else
#  include <fcntl.h>          // POSIX: make stdin non-blocking
#endif
#include <time.h>
#include <stdlib.h>

#ifdef _WIN32
static void usleep(unsigned int us) { Sleep(us / 1000); }   // Sleep(0)=yield for sub-ms naps
static bool stdinHasData() {                                 // fd 0 is a pipe in the tests
  HANDLE h = (HANDLE)_get_osfhandle(0);
  DWORD avail = 0;
  if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) return true;  // not a pipe: just block
  return avail > 0;
}
#endif

SDClass SD;
using namespace UsbSync;

struct PipeIO : IO {
  PipeIO() {
#ifdef _WIN32
    // The CRT opens stdin/stdout in TEXT mode: Ctrl-Z would look like EOF and \r\n
    // would be translated - fatal for a binary protocol over pipes.
    _setmode(0, _O_BINARY);
    _setmode(1, _O_BINARY);
#else
    fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
#endif
  }
  size_t readSome(uint8_t* b, size_t n) override {
#ifdef _WIN32
    if (!stdinHasData()) return 0;                     // empty pipe: behave like O_NONBLOCK
#endif
    ssize_t k = read(0, b, n);
    if (k == 0) exit(0);                               // PC closed the pipe
    return k > 0 ? (size_t)k : 0;
  }
  void write(const uint8_t* b, size_t n) override {
    size_t off = 0;
    while (off < n) { ssize_t k = ::write(1, b + off, n - off); if (k > 0) off += k; else usleep(100); }
  }
  uint32_t ms() override {
#ifdef _WIN32
    static const long long freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (uint32_t)(c.QuadPart * 1000 / freq);
#else
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000u + t.tv_nsec / 1000000u;
#endif
  }
  void setBaud(uint32_t b) override { FILE* f = fopen((SD.root + "/baud.txt").c_str(), "w"); if (f) { fprintf(f, "%u", b); fclose(f); } }
};

static void libChanged() { FILE* f = fopen((SD.root + "/libdirty.txt").c_str(), "w"); if (f) { fputs("1", f); fclose(f); } }
static void doExit() { usleep(50000); exit(0); }

int main(int argc, char** argv) {
  SD.root = argv[1];
#ifdef _WIN32
  mkdir((SD.root + "/music").c_str());
#else
  mkdir((SD.root + "/music").c_str(), 0755);
#endif
  PipeIO io;
  Engine eng(io);
  eng.sdOk = true;
  eng.onLibChanged = libChanged;
  eng.onExit = doExit;
  for (;;) { eng.poll(); usleep(200); }
}
