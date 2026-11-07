#include "settings.h"
#include "config.h"
#include <Preferences.h>

Settings S;
const uint16_t TIMEOUT_SEC[5]  = {0, 15, 30, 60, 120};
const uint16_t CPU_MHZ[3]      = {80, 160, 240};
const uint16_t POWEROFF_MIN[4] = {0, 5, 10, 30};

static Preferences pref;
static bool dirty = false;
static uint32_t dirtyAt = 0;

static void defaults() {
  memset(&S, 0, sizeof(S));
  S.ver = 3;
  S.brightness = 70;
  S.timeoutIdx = 2;
  S.cpuIdx = 1;
  S.powerOffIdx = 2;
  S.animations = 1;
  S.repeat = 1;
  S.volume = 40;
  S.volLimit = 80;
  strlcpy(S.btName, DEFAULT_BT_NAME, sizeof(S.btName));
}

void Cfg::load() {
  defaults();
  pref.begin("mypod", false);
  Settings t;
  if (pref.getBytes("cfg", &t, sizeof(t)) == sizeof(t) && t.ver == S.ver) S = t;
  pref.end();
}

void Cfg::save() {
  pref.begin("mypod", false);
  pref.putBytes("cfg", &S, sizeof(S));
  pref.end();
  dirty = false;
}

void Cfg::saveSoon() { dirty = true; dirtyAt = millis(); }

void Cfg::tick() {
  if (dirty && millis() - dirtyAt > 2000) save();
}

// library-changed flag, kept directly in NVS so USB sync mode can set it
// and the next player boot picks it up
void Cfg::setLibDirty(bool v) {
  Preferences p;
  p.begin("mypod", false);
  p.putUChar("libdirty", v ? 1 : 0);
  p.end();
}

bool Cfg::libDirty() {
  Preferences p;
  p.begin("mypod", true);
  bool v = p.getUChar("libdirty", 0) != 0;
  p.end();
  return v;
}
