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

#ifdef _WIN32
#include <windows.h>

// MinGW has no BSD strlcpy (src/usbsync_engine.h uses it) - host-build fallback.
static inline size_t strlcpy(char* dst, const char* src, size_t n) {
  size_t len = strlen(src);
  if (n) { size_t c = len < n - 1 ? len : n - 1; memcpy(dst, src, c); dst[c] = 0; }
  return len;
}

// The pod stores UTF-8 names but the narrow MinGW CRT interprets paths with the
// ANSI codepage, so every OS call here goes through the wide API with explicit
// UTF-8 <-> UTF-16 conversion.
static inline std::wstring wideOf(const std::string& u8) {
  if (u8.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), &w[0], n);
  return w;
}
static inline std::string utf8Of(const std::wstring& w) {
  if (w.empty()) return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  if (n <= 0) return std::string();
  std::string s((size_t)n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
  return s;
}
static inline std::wstring nativeW(const std::string& u8) {   // forward -> back slashes
  std::wstring w = wideOf(u8);
  for (size_t i = 0; i < w.size(); i++) if (w[i] == L'/') w[i] = L'\\';
  return w;
}
#endif

#define PATH_MAX_LEN 192
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
#ifdef _WIN32
  HANDLE fh = INVALID_HANDLE_VALUE;    // open write handle (engine-opened files only)
  std::wstring pat;                    // find-first pattern (directory listings only)
  WIN32_FIND_DATAW fd {};
  bool isdir = false, ok = false;
#else
  FILE* fp = nullptr;
  DIR* dp = nullptr;
  bool dir = false;
#endif
  std::string full, virt;
 public:
  File() {}
#ifdef _WIN32
  static File realFile(const std::string& r, const std::string& v) {
    File f; f.full = r; f.virt = v; f.ok = true; return f;
  }
  static File dirFile(const std::string& r, const std::string& v) {
    File f; f.full = r; f.virt = v; f.isdir = true; f.ok = true;
    f.pat = nativeW(r + "\\*");
    return f;
  }
  static File writeHandle(HANDLE h, const std::string& r, const std::string& v) {
    File f; f.fh = h; f.full = r; f.virt = v; f.ok = true; return f;
  }
#else
  File(FILE* f, const std::string& real, const std::string& v) : fp(f), full(real), virt(v) {}
  File(DIR* d, const std::string& real, const std::string& v) : dp(d), full(real), virt(v), dir(true) {}
#endif
  explicit operator bool() const {
#ifdef _WIN32
    return ok;
#else
    return fp || dp;
#endif
  }
  size_t write(const uint8_t* b, size_t n) {
#ifdef _WIN32
    if (fh == INVALID_HANDLE_VALUE) return 0;
    size_t off = 0;
    while (off < n) {
      DWORD w = 0;
      if (!WriteFile(fh, b + off, (DWORD)(n - off), &w, nullptr) || w == 0) break;
      off += w;
    }
    return off;
#else
    return fp ? fwrite(b, 1, n, fp) : 0;
#endif
  }
  void close() {
#ifdef _WIN32
    if (fh != INVALID_HANDLE_VALUE) CloseHandle(fh);
    if (dh != INVALID_HANDLE_VALUE) FindClose(dh);
    fh = INVALID_HANDLE_VALUE;
    dh = INVALID_HANDLE_VALUE;
    pat.clear(); isdir = false; ok = false;
#else
    if (fp) fclose(fp);
    if (dp) closedir(dp);
    fp = nullptr; dp = nullptr;
#endif
  }
  bool isDirectory() const {
#ifdef _WIN32
    return isdir;
#else
    return dir;
#endif
  }
  const char* name() const { size_t p = virt.rfind('/'); return virt.c_str() + (p == std::string::npos ? 0 : p + 1); }
  String path() const { return String(virt); }
  size_t size() const {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(nativeW(full).c_str(), GetFileExInfoStandard, &fa)) return 0;
    return ((size_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
#else
    struct stat st; return stat(full.c_str(), &st) == 0 ? (size_t)st.st_size : 0;
#endif
  }
  File openNextFile();
  std::string root_;

 private:
#ifdef _WIN32
  HANDLE dh = INVALID_HANDLE_VALUE;    // active find handle, opened lazily
#endif
};

struct SDClass {
  std::string root;
  std::string real(const char* p) const { return root + p; }
  File open(const char* p, const char* mode = FILE_READ) {
    std::string r = real(p);
#ifdef _WIN32
    bool wr = strcmp(mode, FILE_WRITE) == 0;
    if (wr) {                              // "wb": create/truncate even if it doesn't exist yet
      HANDLE h = CreateFileW(nativeW(r).c_str(), GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (h == INVALID_HANDLE_VALUE) return File();
      return File::writeHandle(h, r, p);
    }
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(nativeW(r).c_str(), GetFileExInfoStandard, &fa)) return File();
    if (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return File::dirFile(r, p);
    HANDLE h = CreateFileW(nativeW(r).c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return File();
    return File::writeHandle(h, r, p);
#else
    struct stat st;
    if (stat(r.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      DIR* d = opendir(r.c_str());
      File f(d, r, p); f.root_ = root; return f;
    }
    FILE* fp = fopen(r.c_str(), mode);
    if (!fp) return File();
    File f(fp, r, p); f.root_ = root; return f;
#endif
  }
  bool exists(const char* p) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA fa;
    return GetFileAttributesExW(nativeW(real(p)).c_str(), GetFileExInfoStandard, &fa) != 0;
#else
    struct stat st; return stat(real(p).c_str(), &st) == 0;
#endif
  }
#ifdef _WIN32
  bool mkdir(const char* p) { return CreateDirectoryW(nativeW(real(p)).c_str(), nullptr) != 0; }
  bool remove(const char* p) { return DeleteFileW(nativeW(real(p)).c_str()) != 0; }
  bool rename(const char* a, const char* b) { return MoveFileW(nativeW(real(a)).c_str(), nativeW(real(b)).c_str()) != 0; }
#else
  bool mkdir(const char* p) { return ::mkdir(real(p).c_str(), 0755) == 0; }
  bool remove(const char* p) { return ::remove(real(p).c_str()) == 0; }
  bool rename(const char* a, const char* b) { return ::rename(real(a).c_str(), real(b).c_str()) == 0; }
#endif
  uint64_t cardSize() { return 8ULL << 30; }
  uint64_t totalBytes() { return 8ULL << 30; }
  uint64_t usedBytes() { return 1ULL << 30; }
};
extern SDClass SD;

inline File File::openNextFile() {
#ifdef _WIN32
  if (pat.empty()) return File();
  for (;;) {
    if (dh == INVALID_HANDLE_VALUE) {
      dh = FindFirstFileW(pat.c_str(), &fd);
      if (dh == INVALID_HANDLE_VALUE) { pat.clear(); return File(); }
    } else if (!FindNextFileW(dh, &fd)) {
      FindClose(dh);
      dh = INVALID_HANDLE_VALUE;
      pat.clear();
      return File();
    }
    std::wstring wn = fd.cFileName;
    if (wn == L"." || wn == L"..") continue;
    std::string nm = utf8Of(wn);
    std::string r = full + "\\" + nm;
    std::string v = virt + "/" + nm;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return File::dirFile(r, v);
    return File::realFile(r, v);
  }
#else
  if (!dp) return File();
  for (;;) {
    struct dirent* e = readdir(dp);
    if (!e) return File();
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    std::string v = virt + "/" + e->d_name;
    return SD.open(v.c_str());
  }
#endif
}
