// Runs the REAL device protocol engine on the PC. stdin/stdout play the USB serial port.
// usage: host_device <sd-root-dir>
#define USBSYNC_HOST 1
#include "../../src/usbsync_engine.h"
#include <fcntl.h>
#include <time.h>
#include <stdlib.h>

SDClass SD;
using namespace UsbSync;

struct PipeIO : IO {
  PipeIO() { fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK); }
  size_t readSome(uint8_t* b, size_t n) override {
    ssize_t k = read(0, b, n);
    if (k == 0) exit(0);                               // PC closed the pipe
    return k > 0 ? (size_t)k : 0;
  }
  void write(const uint8_t* b, size_t n) override {
    size_t off = 0;
    while (off < n) { ssize_t k = ::write(1, b + off, n - off); if (k > 0) off += k; else usleep(100); }
  }
  uint32_t ms() override { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000u + t.tv_nsec / 1000000u; }
  void setBaud(uint32_t b) override { FILE* f = fopen((SD.root + "/baud.txt").c_str(), "w"); if (f) { fprintf(f, "%u", b); fclose(f); } }
};

static void libChanged() { FILE* f = fopen((SD.root + "/libdirty.txt").c_str(), "w"); if (f) { fputs("1", f); fclose(f); } }
static void doExit() { usleep(50000); exit(0); }

int main(int argc, char** argv) {
  SD.root = argv[1];
  mkdir((SD.root + "/music").c_str(), 0755);
  PipeIO io;
  Engine eng(io);
  eng.sdOk = true;
  eng.onLibChanged = libChanged;
  eng.onExit = doExit;
  for (;;) { eng.poll(); usleep(200); }
}
