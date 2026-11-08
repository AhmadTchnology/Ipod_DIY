#include "btfix.h"
#include <esp_bt.h>
#include <esp_a2dp_api.h>

namespace BtFix {

void tick(BluetoothA2DPSource& a2dp, bool wantAudio) {
  static bool powerDone = false;
  static uint32_t notStartedSince = 0, lastKick = 0;
  uint32_t now = millis();

  // 1) Radio: the profiling report shows the stack pumps audio at ~68 % of real time, i.e. the
  //    link only carries ~30 KB/s. Espressif's default BR/EDR power range is N0..P3 (0..+3 dBm);
  //    allow up to +9 dBm. Keeping min at the default leaves the controller's power control in
  //    charge (it only goes high when the link is weak). Needs the controller to be enabled.
  if (!powerDone && esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED) {
    esp_err_t r = esp_bredr_tx_power_set(ESP_PWR_LVL_N0, ESP_PWR_LVL_P9);
    powerDone = (r == ESP_OK);
    esp_power_level_t lo = ESP_PWR_LVL_N12, hi = ESP_PWR_LVL_N12;
    esp_bredr_tx_power_get(&lo, &hi);
    Serial.printf("[bt] BR/EDR tx power set -> %s (now min=%d max=%d, max level 7 = +9 dBm)\n",
                  r == ESP_OK ? "ok" : "FAILED", (int)lo, (int)hi);
  }

  // 2) The stream should be running whenever a track plays. If the sink has not started /
  //    has stopped it for 2.5 s, ask again (the library only retried every 10 s).
  bool started = a2dp.is_connected() && a2dp.get_audio_state() == ESP_A2D_AUDIO_STATE_STARTED;
  if (!a2dp.is_connected() || started || !wantAudio) { notStartedSince = 0; return; }
  if (!notStartedSince) notStartedSince = now;
  if (now - notStartedSince > 2500 && now - lastKick > 3000) {
    lastKick = now;
    Serial.println("[bt] media stream not running: asking the stack to start it");
    esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
  }
}

}  // namespace BtFix
