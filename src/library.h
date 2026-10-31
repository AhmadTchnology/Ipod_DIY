#pragma once
#include <Arduino.h>

namespace Library {
  void begin();                 // mount SD + scan
  void rescan();
  bool sdOk();
  int  count();
  const char* path(int i);
  // "Artist - Title.mp3" -> artist / title (ASCII only, safe for the built-in font)
  void displayName(int i, char* title, size_t tn, char* artist, size_t an);
}
