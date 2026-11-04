#pragma once
#include <Arduino.h>

struct Settings {
  uint8_t ver;
  uint8_t brightness;    // 5..100
  uint8_t autoBright;
  uint8_t timeoutIdx;    // index into TIMEOUT_SEC
  uint8_t cpuIdx;        // index into CPU_MHZ
  uint8_t statusLed;
  uint8_t powerOffIdx;   // index into POWEROFF_MIN
  uint8_t animations;
  uint8_t shuffle;
  uint8_t repeat;        // 0 off, 1 all, 2 one
  uint8_t volume;        // 0..100
  uint8_t volLimit;      // 20..100
  uint8_t pairNext;      // boot into pairing mode
  char    btName[40];
};

extern Settings S;
extern const uint16_t TIMEOUT_SEC[5];
extern const uint16_t CPU_MHZ[3];
extern const uint16_t POWEROFF_MIN[4];

namespace Cfg {
  void load();
  void save();
  void saveSoon();   // debounced
  void tick();
  void setLibDirty(bool v);  // library changed on the card -> rebuild the index
  bool libDirty();
}
