#pragma once
#include <Arduino.h>

#define FW_NAME    "MyPod"
#define FW_VERSION "1.1.0"

// ---- Screen (portrait) ----
#define SCR_W 240
#define SCR_H 320
#define TFT_ROT 0            // 0 or 2. Use 2 if the picture is upside down.

// Gamma fix: 0 = stock behaviour, 1 = fix reported for this board
// (GAMSET curve 2, then curve 1), 2 = full ST7789-style gamma tables
// (fallback for panels where 1 has no effect).
#define TFT_GAMMA_FIX 1

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
#define PATH_MAX_LEN  192                   // UTF-8 names are longer (Arabic ~2 B/char)
#define RING_BYTES    (6 * 1024)      // PCM ring between decoder and BT callback
                                      // (MyPod runs OK on 6 KB; 16 KB ate ~10 KB
                                      // that SBC + fopen need at play start)
#define DEC_STACK     6144           // decoder task stack (the task never prints)

// Optional battery gauge: wire the LiPo through a 2x100k divider to a free ADC1 pin
// (GPIO35 is broken out on the CYD) and set the pin here. -1 = no sensor.
#define BAT_ADC_PIN   -1
#define BAT_DIV       2.0f          // divider ratio (Vbat = Vpin * BAT_DIV)

// Optional: hard-code your headphones' Bluetooth name (otherwise pair in the UI)
#define DEFAULT_BT_NAME ""
