// Minimal Arduino/SD stand-ins so src/usbsync_engine.h compiles and runs on a PC.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#define PATH_MAX_LEN 128
#define FILE_READ "rb"
#define FILE_WRITE "wb"

class String {
  std::string s;
 public:
  String() {}
  String(const char* c) : s(c) {}
  String(const std::string& c) : s(c) {}
  const char* c_str() const { return s.c_str(); }
  size_t length() const { return s.size(); }
};

class File {
  FILE* fp = nullptr;
  DIR* dp = nullptr;
  std::string full, virt;
  bool dir = false;
 public:
  File() {}
  File(FILE* f, const std::string& real, const std::string& v) : fp(f), full(real), virt(v) {}
  File(DIR* d, const std::string& real, const std::string& v) : dp(d), full(real), virt(v), dir(true) {}
  explicit operator bool() const { return fp || dp; }
  size_t write(const uint8_t* b, size_t n) { return fp ? fwrite(b, 1, n, fp) : 0; }
  void close() { if (fp) fclose(fp); if (dp) closedir(dp); fp = nullptr; dp = nullptr; }
  bool isDirectory() const { return dir; }
  const char* name() const { size_t p = virt.rfind('/'); return virt.c_str() + (p == std::string::npos ? 0 : p + 1); }
  String path() const { return String(virt); }
  size_t size() const { struct stat st; return stat(full.c_str(), &st) == 0 ? st.st_size : 0; }
  File openNextFile();
  std::string root_;
};

struct SDClass {
  std::string root;
  std::string real(const char* p) const { return root + p; }
  File open(const char* p, const char* mode = FILE_READ) {
    std::string r = real(p);
    struct stat st;
    if (stat(r.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      DIR* d = opendir(r.c_str());
      File f(d, r, p); f.root_ = root; return f;
    }
    FILE* fp = fopen(r.c_str(), mode);
    if (!fp) return File();
    File f(fp, r, p); f.root_ = root; return f;
  }
  bool exists(const char* p) { struct stat st; return stat(real(p).c_str(), &st) == 0; }
  bool mkdir(const char* p) { return ::mkdir(real(p).c_str(), 0755) == 0; }
  bool remove(const char* p) { return ::remove(real(p).c_str()) == 0; }
  bool rename(const char* a, const char* b) { return ::rename(real(a).c_str(), real(b).c_str()) == 0; }
  uint64_t cardSize() { return 8ULL << 30; }
  uint64_t totalBytes() { return 8ULL << 30; }
  uint64_t usedBytes() { return 1ULL << 30; }
};
extern SDClass SD;

inline File File::openNextFile() {
  if (!dp) return File();
  for (;;) {
    struct dirent* e = readdir(dp);
    if (!e) return File();
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    std::string v = virt + "/" + e->d_name;
    return SD.open(v.c_str());
  }
}
