// Album-cover drawing. The PC tool ("MyPod Sync") turns each song's embedded cover into a
// 200x200 RGB565 file in /art on the SD card; the player only streams that small file to the
// screen a few rows at a time (about 3 KB of RAM), so it fits next to Bluetooth + the decoder.
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

namespace Art {
  // Draw the cached cover of `songPath` with its left edge at x (may be shifted for the slide
  // animation); only columns inside [clipX0, clipX1) are painted. Returns false when the song
  // has no cover file (the caller draws the placeholder).
  bool exists(const char* songPath);          // a cover file is on the card for this song
  int  colorMode();                            // 0 normal, 1 swap red/blue, 2 invert, 3 both
  void setColorMode(int m);
  bool drawCover(TFT_eSPI& g, const char* songPath, int x, int y, int clipX0, int clipX1);
}
