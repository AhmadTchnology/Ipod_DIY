// =====================================================================
//  MyPod USB sync protocol - device side (header-only, no UI, no Serial).
//
//  Frame (both directions):
//      A5 5A | cmd | len (u16 LE) | payload[len] | crc16 (u16 LE)
//  crc16 = CRC-16/CCITT-FALSE over cmd, len and payload.
//  Replies use cmd|0x80 (ACK) or 0xFF (error: [orig cmd][code][text]).
//  Transfers are stop-and-wait (one 4 KB chunk, one ACK) so the PC can
//  never overrun the pod, whatever the SD card is doing.
//
//  The engine is I/O agnostic: usbsync.cpp feeds it the real Serial port,
//  the PC test-suite feeds it a pipe (see pc_sync/tests).
// =====================================================================
#pragma once
#ifdef USBSYNC_HOST
#include "host_shim.h"
#else
#include <Arduino.h>
#include <SD.h>
#include "config.h"
#endif
#include "arthash.h"
#include <string.h>

namespace UsbSync {

enum : uint8_t {
  CMD_HELLO = 0x01, CMD_SETBAUD = 0x02, CMD_PING = 0x03, CMD_FREE = 0x04,
  CMD_PUT_BEGIN = 0x10, CMD_PUT_DATA = 0x11, CMD_PUT_END = 0x12, CMD_PUT_ABORT = 0x13,
  CMD_LIST = 0x20, CMD_DELETE = 0x21,
  CMD_EXIT = 0x30,
  R_LISTENT = 0xA0, R_LISTDONE = 0xA1, R_ERROR = 0xFF
};
enum : uint8_t {
  E_CMD = 1, E_PATH = 2, E_SD = 3, E_STATE = 5, E_OFFSET = 6, E_CRC = 7, E_SIZE = 8, E_BAUD = 9
};
static const uint8_t PROTO_VER = 1;
static const size_t MAX_PAY = 4200;               // 4096-byte chunk + 4-byte offset + slack

struct IO {
  virtual size_t readSome(uint8_t* b, size_t n) = 0;   // non-blocking: bytes available now (0 = none)
  virtual void write(const uint8_t* b, size_t n) = 0;
  virtual uint32_t ms() = 0;
  virtual void setBaud(uint32_t baud) = 0;
  virtual ~IO() {}
};

struct Status {
  bool     linked = false;            // a valid frame arrived in the last few seconds
  uint32_t lastFrameMs = 0;
  char     name[48] = "";             // file being received
  uint32_t got = 0, total = 0;
  uint32_t songs = 0;                 // mp3 files completed this session
  char     err[40] = "";
};

static inline uint16_t crc16Step(uint16_t c, uint8_t b) {
  c ^= (uint16_t)b << 8;
  for (int i = 0; i < 8; i++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
  return c;
}
static inline uint32_t crc32Update(uint32_t c, const uint8_t* d, size_t n) {   // zlib CRC-32, c starts 0
  c = ~c;
  while (n--) {
    c ^= *d++;
    for (int i = 0; i < 8; i++) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : (c >> 1);
  }
  return ~c;
}
static inline uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline void wr32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static inline bool endsCI(const char* s, const char* suf) {
  size_t a = strlen(s), b = strlen(suf);
  return a >= b && strcasecmp(s + a - b, suf) == 0;
}
static inline bool okChars(const char* p) {
  for (; *p; p++) {
    unsigned char c = (unsigned char)*p;
    if (c < 0x20 || c > 0x7E || strchr("\"*:<>?|\\", c)) return false;
  }
  return true;
}
static inline bool validPath(const char* p, const char* prefix, const char* ext) {
  size_t l = strlen(p);
  return l > strlen(prefix) + 4 && l < (size_t)PATH_MAX_LEN - 8 && !strncmp(p, prefix, strlen(prefix)) &&
         !strstr(p, "..") && !strstr(p, "//") && okChars(p) && endsCI(p, ext);
}

class Engine {
 public:
  Engine(IO& io) : io_(io) {}
  Status st;
  bool sdOk = false;
  void (*onLibChanged)() = nullptr;     // songs were added / removed
  void (*onExit)() = nullptr;           // PC asked to go back to the player (called after the ACK)

  // Call often. Parses whatever bytes are waiting and answers complete frames.
  void poll() {
    uint8_t tmp[256];
    uint32_t now = io_.ms();
    if (state_ != 0 && now - lastByte_ > 1500) state_ = 0;           // half a frame, then silence
    size_t n = io_.readSome(tmp, sizeof(tmp));
    for (size_t i = 0; i < n; i++) feed(tmp[i]);
    if (n) lastByte_ = io_.ms();
    if (st.linked && now - st.lastFrameMs > 4000) st.linked = false;
    if (baudPending_ && now > baudRevertAt_) {                        // PC never confirmed the new speed
      io_.setBaud(115200);
      baudPending_ = false;
    }
  }

  void abortPut() {
    if (putOpen_) { put_.close(); putOpen_ = false; }
    if (putTmp_[0]) SD.remove(putTmp_);
    putTmp_[0] = 0;
    st.name[0] = 0; st.got = st.total = 0;
  }

 private:
  IO& io_;
  // parser
  uint8_t fb_[MAX_PAY + 8];
  int state_ = 0;                  // 0 magic1, 1 magic2, 2 header, 3 payload, 4 crc
  uint8_t cmd_ = 0;
  uint16_t len_ = 0, pos_ = 0, crc_ = 0;
  uint8_t hdr_[3];
  uint8_t crcb_[2];
  uint32_t lastByte_ = 0;
  // put session
  File put_;
  bool putOpen_ = false;
  char putPath_[PATH_MAX_LEN] = "";
  char putTmp_[PATH_MAX_LEN + 8] = "";
  uint32_t putSize_ = 0, putCrc32_ = 0;
  // baud change
  bool baudPending_ = false;
  uint32_t baudRevertAt_ = 0;

  void feed(uint8_t b) {
    switch (state_) {
      case 0: if (b == 0xA5) state_ = 1; break;
      case 1: state_ = (b == 0x5A) ? 2 : (b == 0xA5 ? 1 : 0); pos_ = 0; crc_ = 0xFFFF; break;
      case 2:
        hdr_[pos_++] = b;
        crc_ = crc16Step(crc_, b);
        if (pos_ == 3) {
          cmd_ = hdr_[0];
          len_ = hdr_[1] | (hdr_[2] << 8);
          pos_ = 0;
          if (len_ > MAX_PAY) state_ = 0;
          else state_ = len_ ? 3 : 4;
        }
        break;
      case 3:
        fb_[pos_++] = b;
        crc_ = crc16Step(crc_, b);
        if (pos_ == len_) { pos_ = 0; state_ = 4; }
        break;
      case 4:
        crcb_[pos_++] = b;
        if (pos_ == 2) {
          state_ = 0;
          if ((uint16_t)(crcb_[0] | (crcb_[1] << 8)) == crc_) {
            st.linked = true;
            st.lastFrameMs = io_.ms();
            baudPending_ = false;                                     // a valid frame: link is fine
            handle(cmd_, fb_, len_);
          }
        }
        break;
    }
  }

  void send(uint8_t cmd, const uint8_t* pl, uint16_t len) {
    uint8_t buf[64 + PATH_MAX_LEN];
    if (len > sizeof(buf) - 7) len = sizeof(buf) - 7;
    buf[0] = 0xA5; buf[1] = 0x5A; buf[2] = cmd; buf[3] = len & 0xFF; buf[4] = len >> 8;
    if (len) memcpy(buf + 5, pl, len);
    uint16_t c = 0xFFFF;
    for (size_t i = 2; i < 5u + len; i++) c = crc16Step(c, buf[i]);
    buf[5 + len] = c & 0xFF; buf[6 + len] = c >> 8;
    io_.write(buf, 7u + len);
  }
  void ack(uint8_t cmd, const uint8_t* pl = nullptr, uint16_t len = 0) { send(cmd | 0x80, pl, len); }
  void fail(uint8_t cmd, uint8_t code, const char* text) {
    uint8_t b[2 + 40];
    b[0] = cmd; b[1] = code;
    size_t l = strlen(text); if (l > 38) l = 38;
    memcpy(b + 2, text, l);
    send(R_ERROR, b, (uint16_t)(2 + l));
    strlcpy(st.err, text, sizeof(st.err));
  }

  static void mkdirs(const char* path) {                              // create every parent folder
    char p[PATH_MAX_LEN];
    strlcpy(p, path, sizeof(p));
    for (char* s = p + 1; *s; s++) {
      if (*s == '/') { *s = 0; if (!SD.exists(p)) SD.mkdir(p); *s = '/'; }
    }
  }

  void libChanged() { if (onLibChanged) onLibChanged(); }

  void handle(uint8_t cmd, const uint8_t* pl, uint16_t len) {
    switch (cmd) {
      case CMD_PING: ack(cmd); break;

      case CMD_HELLO: {
        const char* name = "MyPod";
        uint8_t b[16];
        b[0] = PROTO_VER;
        b[1] = sdOk ? 1 : 0;
        uint32_t mb = sdOk ? (uint32_t)(SD.cardSize() / (1024ULL * 1024ULL)) : 0;
        wr32(b + 2, mb);
        b[6] = (uint8_t)strlen(name);
        memcpy(b + 7, name, b[6]);
        ack(cmd, b, 7 + b[6]);
        break;
      }

      case CMD_FREE: {                                                // slow on big cards (scans the FAT)
        uint8_t b[8] = {0};
        if (sdOk) {
          wr32(b, (uint32_t)(SD.totalBytes() / 1024ULL));
          wr32(b + 4, (uint32_t)((SD.totalBytes() - SD.usedBytes()) / 1024ULL));
        }
        ack(cmd, b, 8);
        break;
      }

      case CMD_SETBAUD: {
        if (len != 4) { fail(cmd, E_CMD, "bad baud frame"); break; }
        uint32_t baud = rd32(pl);
        if (baud != 115200 && baud != 230400 && baud != 460800 && baud != 921600 &&
            baud != 1500000 && baud != 2000000) { fail(cmd, E_BAUD, "baud not allowed"); break; }
        ack(cmd, pl, 4);                                              // answered at the old speed
        io_.setBaud(baud);
        baudPending_ = (baud != 115200);                             // revert if PC never talks at the new speed
        baudRevertAt_ = io_.ms() + 5000;
        break;
      }

      case CMD_PUT_BEGIN: {
        if (!sdOk) { fail(cmd, E_SD, "no SD card"); break; }
        if (len < 6) { fail(cmd, E_CMD, "short begin"); break; }
        uint32_t size = rd32(pl);
        uint8_t pl8 = pl[4];
        if ((uint16_t)(5 + pl8) != len || pl8 == 0 || pl8 >= PATH_MAX_LEN) { fail(cmd, E_PATH, "bad path length"); break; }
        char path[PATH_MAX_LEN];
        memcpy(path, pl + 5, pl8);
        path[pl8] = 0;
        bool song = validPath(path, "/music/", ".mp3");
        bool cover = validPath(path, "/art/", ".cov");
        if (!song && !cover) { fail(cmd, E_PATH, "path not allowed"); break; }
        abortPut();
        mkdirs(path);
        strlcpy(putPath_, path, sizeof(putPath_));
        snprintf(putTmp_, sizeof(putTmp_), "%s.tmp", path);
        SD.remove(putTmp_);
        put_ = SD.open(putTmp_, FILE_WRITE);
        if (!put_) { putTmp_[0] = 0; fail(cmd, E_SD, "cannot create file"); break; }
        putOpen_ = true;
        putSize_ = size; putCrc32_ = 0;
        st.got = 0; st.total = size;
        const char* nm = strrchr(path, '/');
        strlcpy(st.name, nm ? nm + 1 : path, sizeof(st.name));
        st.err[0] = 0;
        ack(cmd);
        break;
      }

      case CMD_PUT_DATA: {
        if (!putOpen_) { fail(cmd, E_STATE, "no file open"); break; }
        if (len < 5) { fail(cmd, E_CMD, "short data"); break; }
        uint32_t off = rd32(pl);
        uint16_t dl = len - 4;
        const uint8_t* d = pl + 4;
        if (off + dl == st.got && off < st.got) {                    // PC re-sent a chunk we already have
          uint8_t b[4]; wr32(b, st.got); ack(cmd, b, 4); break;
        }
        if (off != st.got) { fail(cmd, E_OFFSET, "bad offset"); break; }
        if (st.got + dl > putSize_) { fail(cmd, E_SIZE, "more data than announced"); break; }
        if (put_.write(d, dl) != dl) { abortPut(); fail(cmd, E_SD, "SD write failed (card full?)"); break; }
        putCrc32_ = crc32Update(putCrc32_, d, dl);
        st.got += dl;
        uint8_t b[4]; wr32(b, st.got);
        ack(cmd, b, 4);
        break;
      }

      case CMD_PUT_END: {
        if (!putOpen_) { fail(cmd, E_STATE, "no file open"); break; }
        if (len != 4) { fail(cmd, E_CMD, "bad end frame"); break; }
        if (st.got != putSize_) { abortPut(); fail(cmd, E_SIZE, "size mismatch"); break; }
        if (rd32(pl) != putCrc32_) { abortPut(); fail(cmd, E_CRC, "crc mismatch"); break; }
        put_.close();
        putOpen_ = false;
        SD.remove(putPath_);
        if (!SD.rename(putTmp_, putPath_)) { SD.remove(putTmp_); putTmp_[0] = 0; fail(cmd, E_SD, "rename failed"); break; }
        putTmp_[0] = 0;
        if (endsCI(putPath_, ".mp3")) { st.songs++; libChanged(); }
        st.name[0] = 0; st.got = st.total = 0;
        ack(cmd);
        break;
      }

      case CMD_PUT_ABORT: abortPut(); ack(cmd); break;

      case CMD_LIST: {
        uint32_t count = 0;
        if (sdOk) listDir("/music", 3, count);
        uint8_t b[4]; wr32(b, count);
        send(R_LISTDONE, b, 4);
        break;
      }

      case CMD_DELETE: {
        char path[PATH_MAX_LEN];
        if (len == 0 || len >= PATH_MAX_LEN) { fail(cmd, E_PATH, "bad path"); break; }
        memcpy(path, pl, len);
        path[len] = 0;
        if (!validPath(path, "/music/", ".mp3")) { fail(cmd, E_PATH, "path not allowed"); break; }
        if (!SD.exists(path)) { fail(cmd, E_PATH, "no such file"); break; }
        if (!SD.remove(path)) { fail(cmd, E_SD, "delete failed (file busy on the card?)"); break; }
        char cp[40];
        artCachePath(path, cp, sizeof(cp));   // cover removal is best effort: fine if it already went
        SD.remove(cp);
        libChanged();
        ack(cmd);
        break;
      }

      case CMD_EXIT:
        abortPut();
        ack(cmd);
        if (onExit) onExit();
        break;

      default: fail(cmd, E_CMD, "unknown command"); break;
    }
  }

  void listDir(const char* dir, int depth, uint32_t& count) {
    File d = SD.open(dir);
    if (!d) return;
    if (!d.isDirectory()) { d.close(); return; }
    for (;;) {
      File e = d.openNextFile();
      if (!e) break;
      const char* nm = e.name();
      if (nm[0] == '.') { e.close(); continue; }
      if (e.isDirectory()) {
        String sub = e.path();
        e.close();
        if (depth > 0) listDir(sub.c_str(), depth - 1, count);
      } else if (endsCI(nm, ".mp3")) {
        String p = e.path();
        uint32_t sz = (uint32_t)e.size();
        e.close();
        if (p.length() >= PATH_MAX_LEN) continue;
        char cp[40];
        artCachePath(p.c_str(), cp, sizeof(cp));
        uint8_t b[5 + PATH_MAX_LEN];
        wr32(b, sz);
        b[4] = SD.exists(cp) ? 1 : 0;
        memcpy(b + 5, p.c_str(), p.length());
        send(R_LISTENT, b, (uint16_t)(5 + p.length()));
        count++;
      } else {
        e.close();
      }
    }
    d.close();
  }
};

}  // namespace UsbSync
