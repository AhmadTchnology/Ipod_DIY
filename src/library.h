#pragma once
#include <Arduino.h>

namespace Library {
  void begin();                 // mount SD + load (or build) /music.idx
  void rescan();                // rebuild the /music.idx index (stops playback first!)
  bool sdOk();
  int  count();
  const char* path(int i);
  // "Artist - Title.mp3" -> artist / title (ASCII only, safe for the built-in font)
  void displayName(int i, char* title, size_t tn, char* artist, size_t an);
  // serialise SD/FATFS access between the decoder task and the UI task
  void sdEnter();
  void sdExit();
}
