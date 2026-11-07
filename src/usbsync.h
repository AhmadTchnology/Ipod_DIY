#pragma once
#include <Arduino.h>

// USB sync boot mode: the pod stops Bluetooth/player, shows a status screen and
// talks the MyPod sync protocol over the USB serial port, so the PC tool can copy
// songs (+ covers) onto the SD card. Never returns; clears its flag and restarts
// when the PC sends "exit" or the user holds BOOT for 3 s.
namespace UsbSync {
  bool enabled();            // NVS flag ("mypod"/"usb")
  void setEnabled(bool on);
  void run();
}
