#pragma once
#include <Arduino.h>

// WiFi receiver boot mode: captive-portal hotspot + Telegram MP3 fetch.
// Never returns; clears its flag and restarts when the user exits.
namespace Receiver {
  bool enabled();   // NVS flag read ("mypod"/"wifi")
  void setEnabled(bool on);  // write it
  void run();
}
