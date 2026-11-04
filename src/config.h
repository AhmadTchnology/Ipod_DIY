#pragma once
#include <Arduino.h>

#define FW_NAME    "MyPod"
#define FW_VERSION "1.0.0"

// ---- Screen (portrait) ----
#define SCR_W 240
#define SCR_H 320
#define TFT_ROT 0            // 0 or 2. Use 2 if the picture is upside down.

// ---- Fixed by the CYD PCB ----
#define PIN_BL   21          // backlight (PWM)
#define T_CLK    25          // XPT2046 touch, bit-banged (own pins)
#define T_MOSI   32
#define T_MISO   39
#define T_CS     33
#define T_IRQ    36          // low while touched, also the deep-sleep wake pin
#define SD_SCK   18          // microSD on VSPI
#define SD_MISO  19
#define SD_MOSI  23
#define SD_CS    5
#define LED_R    4           // RGB LED, active LOW
#define LED_G    16
#define LED_B    17
#define PIN_LDR  34          // light sensor
#define PIN_BOOT 0           // hold at power-up = re-run touch calibration
#define LDR_DARK_IS_HIGH 1   // flip to 0 if auto-brightness works backwards

// ---- Music / audio ----
#define MUSIC_DIR     "/music"
#define MAX_TRACKS    800
#define PATH_MAX_LEN  128
#define RING_BYTES    (6 * 1024)    // PCM buffer between decoder and Bluetooth

// Optional: hard-code your headphones' Bluetooth name (otherwise pair in the UI)
#define DEFAULT_BT_NAME ""

// ---- Bluetooth start gate ----------------------------------------------
// 1 while the Bluetooth stack is coming up (controller init + bluedroid +
// stack-up, ~2-3 s). Allocation-heavy paths (SD mount, index load, page
// rebuilds, file open) must wait while it is set: bluedroid's init has no
// failure recovery - a failed allocation becomes a memset(NULL) StoreProhibited
// that reboots the whole board. Defined in player.cpp.
extern volatile unsigned char g_btIniting;
