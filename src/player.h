#pragma once
#include <Arduino.h>

namespace Player {
  void prealloc();              // call EARLY in setup(): pin the helix buffers
  void begin();                 // decoder task + PCM ring (cheap; no Bluetooth)
  void ensureBt();              // start the Bluetooth stack on first play
  bool btStarted();             // ensureBt() has run
  void update();                // call from loop(): track-end handling
  void play(int idx);
  void toggle();
  void next();
  void prev();
  void stop();
  void seek(float frac);        // 0..1

  bool active();                // audio is actually playing
  bool paused();
  bool loaded();
  int  current();
  uint32_t serial();            // changes whenever a new track starts
  float position();             // seconds
  uint32_t duration();          // seconds (estimated from bitrate), 0 = unknown
  float fraction();

  void setVolume(uint8_t v);
  uint8_t volume();
  void setShuffle(bool on);
  void setRepeat(uint8_t mode); // 0 off, 1 all, 2 one
  void meta(char* title, size_t tn, char* artist, size_t an);

  bool btConnected();
  bool scanMode();              // true while picking headphones
  int  scanCount();
  const char* scanName(int i);
}
