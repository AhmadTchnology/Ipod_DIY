// Bluetooth link helpers, called from Player::update() (patch notes in library.patch / README).
#pragma once
#include <Arduino.h>
#include <BluetoothA2DPSource.h>

namespace BtFix {
  // wantAudio = a track is loaded and not paused. Cheap; call every UI loop.
  void tick(BluetoothA2DPSource& a2dp, bool wantAudio);
}
