// Bluetooth link helpers, called from Player::update() (see lib patch notes in BT_FIX.md).
#pragma once
#include <Arduino.h>
#include <BluetoothA2DPSource.h>

namespace BtFix {
  // wantAudio = a track is loaded and not paused. Cheap; call every UI loop.
  void tick(BluetoothA2DPSource& a2dp, bool wantAudio);
}
