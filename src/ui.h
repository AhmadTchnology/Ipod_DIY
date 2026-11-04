#pragma once

// Immediate-mode UI drawn straight to the TFT with TFT_eSPI (no widget toolkit,
// no framebuffer): ~0 KB of RAM for the UI itself, only changed regions repaint.
namespace UI {
  void begin();
  void update();            // call every loop(): touch, gestures, repaint
  void setActive(bool on);  // false while the screen is dark; true repaints
  int  idleDelayMs();       // how long loop() may sleep (short while animating)
}
