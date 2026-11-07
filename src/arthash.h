// Where a song's cached cover lives on the SD card. The PC tool computes the same
// name (FNV-1a over the song's full path, e.g. "/music/Artist - Title.mp3").
#pragma once
#include <stdint.h>
#include <stdio.h>

static inline void artCachePath(const char* song, char* out, size_t n) {
  uint32_t h = 2166136261u;
  for (const char* p = song; *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
  snprintf(out, n, "/art/%08lx.cov", (unsigned long)h);
}
