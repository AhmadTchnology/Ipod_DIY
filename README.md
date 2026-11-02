# MyPod — an iPod-style music player for the ESP32 "Cheap Yellow Display"

MyPod turns the **ESP32-2432S028** (a.k.a. CYD / "Cheap Yellow Display") into a
standalone, pocket-sized MP3 player:

- Music from a microSD card plays over **Bluetooth headphones / speakers**
  (A2DP source — the pod *is* the audio source).
- A smooth **240x320 LVGL touch UI** styled like a modern music app.
- A **WiFi receiver mode** that opens its own hotspot + web portal, so a phone
  can hand it your home WiFi and a **Telegram bot token** — then any MP3 sent
  to that bot lands on the SD card and shows up in the library.

No phone app, no cloud dependency for playback — just the pod, an SD card and
Bluetooth earphones.

---

## Features

| Area | What you get |
|---|---|
| **Library** | Scans `/music` on the SD card (up to 3 sub-folder levels, 800 tracks). Smooth-scrolling list; only 8 row widgets are reused, so hundreds of songs cost almost no RAM. Tap a song to play. |
| **Now playing** | "Breathing" album art placeholder, scrolling title, seek bar, elapsed/remaining time, prev / play-pause / next, shuffle, repeat (off / all / one), volume. |
| **Bluetooth** | A2DP source with auto-reconnect. First run opens a pairing picker; the chosen device is remembered and reconnected on every boot. |
| **Theme** | 4 looks — **Dark** (original), Midnight, Slate, **Light** — plus 8 accent colours (Red, Blue, Purple, Green, Orange, Pink, Cyan, Gold). The whole UI recolours: tab bar, sliders, switches, indicators, dialogs. |
| **WiFi receiver** | Separate boot mode (see below): hotspot + captive portal + Telegram bot fetcher. MP3s the bot receives are saved to `/music/telegram/` on the SD card. |
| **Power saving** | Brightness with gamma curve, auto-brightness via the onboard light sensor, screen-off timeout (LVGL stops redrawing, music keeps playing), CPU speed 80/160/240 MHz, auto deep-sleep when idle, manual sleep / screen-off. |
| **Touch** | XPT2046 resistive touch, bit-banged on its own pins, with a 3-target calibration on first boot. |
| **Audio engine** | Helix MP3 decoder (fixed-point, CPU-friendly) on core 0 → PCM ring buffer → A2DP callback on the BT task. Sample rates are resampled on the fly. |

## Hardware

| Part | Detail |
|---|---|
| Board | ESP32-2432S028 2-USB version (2.8", ILI9341-family panel, XPT2046 touch) |
| Display | ILI9341-class driver over HSPI, 40 MHz, portrait 240x320 |
| Touch | XPT2046 bit-banged: CLK 25, MOSI 32, MISO 39, CS 33, IRQ 36 |
| SD card | VSPI: SCK 18, MISO 19, MOSI 23, CS 5 — FAT32 |
| RGB LED | R 4, G 16, B 17 (active LOW) — used as the status LED |
| Light sensor | GPIO 34 (auto-brightness) |
| BOOT button | GPIO 0 — held at power-up re-runs touch calibration |

All pins live in `src/config.h` if your board revision differs.

**Power:** the CYD has no charger or fuel gauge. Feed 5 V (USB or a
charger+boost module). Keep it ≤ 5 V. Battery percentage would need a voltage
divider on a free ADC pin — not implemented.

## Build & flash

1. Install [VS Code](https://code.visualstudio.com/) + the
   [PlatformIO extension](https://platformio.org/).
2. Open this folder — PlatformIO reads `platformio.ini` and installs the
   pinned toolchain and libraries on first build.
3. Plug the pod in, then **Upload** (or `pio run -t upload`). Serial monitor
   is 115200 baud.

Pinned dependencies (they matter — the BT audio stack breaks on newer cores):

| Library | Version |
|---|---|
| Platform | `espressif32@6.9.0` (Arduino-ESP32 2.x — **not** 3.x) |
| LVGL | `~8.3.11` |
| TFT_eSPI | `^2.5.43` |
| ESP32-A2DP | pschatzmann, master (**vendored, patched** — see below) |
| arduino-libhelix | pschatzmann, master |

### Vendored ESP32-A2DP patch

`lib/ESP32-A2DP` is the pschatzmann library vendored at upstream commit
`7da8b6e` (src + metadata, docs/examples removed) and patched for
Bluetooth stability. It is a **project-local lib** (no `lib_deps` entry),
so PlatformIO never reinstalls or overwrites it — fresh clones of this
repo get the patched code automatically:

1. `BluetoothA2DPSource.cpp → av_hdl_stack_evt()` — the AVRCP **controller**
   init is skipped (a source-only pod never sends AVRCP commands to the
   sink); its Bluedroid allocations were starving the A2DP connect burst.
2. The heart-beat timer start skips instead of asserting when
   `xTimerCreate` returns NULL under heap pressure.

TFT pins/flags are set in `platformio.ini` via `USER_SETUP_LOADED` — no
`TFT_eSPI_User_Setup.h` needed. Flash layout is `huge_app.csv` (the firmware
is ~2.1 MB with WiFi + TLS included).

## First boot

1. **Touch calibration** — tap the three targets. Saved to flash; redo via
   holding **BOOT** at power-up or Settings → Recalibrate touch.
2. **Pairing** — with no saved headphones, a pair screen opens. Put your
   earphones in pairing mode and tap their name. The pod restarts and
   reconnects automatically from then on.

## Usage

Bottom tab bar: **Library** (list icon) · **Now playing** (note icon) ·
**Settings** (gear icon).

### Settings

| Setting | Effect |
|---|---|
| Brightness / Auto brightness | Backlight is the biggest battery consumer. |
| Screen off after | Backlight off + LVGL stops redrawing; music keeps playing. |
| CPU speed | 160 MHz default; 80 MHz saves power (watch for audio glitches). |
| Status LED | On = solid blue when headphones are connected, slow blink while searching, fully dark when disabled or the screen is off. |
| Animations | Off = no breathing/fades (less CPU). |
| Auto power-off | Deep sleep after N idle minutes when not playing. Touch wakes it. |
| Sleep now / Turn screen off now | Manual triggers for the above. |
| Volume limit | Caps maximum volume (hearing protection). |
| Rescan music library | Re-scans the SD card. |
| Recalibrate touch | Restart into the 3-target calibration. |

### Theme (top card)

- **Look** — Dark, Midnight, Slate, Light. Choosing one restarts the pod
  (the palette is baked in at boot).
- **Accent colour** — eight colour dots; the active one is ringed. Also
  restarts to apply.

Look + accent are stored as their own NVS keys, so flashing new firmware never
resets them, and your other settings survive untouched.

### WiFi receiver mode (Telegram → SD card)

Because the normal player keeps almost no heap free (LVGL + Bluetooth), the
receiver runs as a **separate boot mode** with LVGL and Bluetooth left out —
that frees enough RAM for TLS.

Turning on **Settings → WIFI RECEIVER** restarts the pod into receiver mode:

1. The pod opens a hotspot **`MyPod-XXXX`**, password **`12345678`**
   (shown on its screen).
2. Connect a phone to that hotspot and open **http://192.168.4.1**:
   - home WiFi **name + password**
   - **Telegram bot token** (from [@BotFather](https://t.me/BotFather))
3. Save — the pod joins your home network and long-polls the bot.
4. Send an MP3 to your bot in Telegram (as a file or audio attachment).
   It's saved to **`/music/telegram/`** on the SD card and you get a
   confirmation message in the chat.
5. Exit: hold the **BOOT** button 3 s, use the **/exit** button on the
   status page, or type `exit` on the USB serial console. The pod restarts
   back into the player; **Rescan music library** picks up the new songs.

The status page (`/status`) auto-refreshes and shows connection state, token
validity, SD-card presence and how many MP3s were received. The pod's screen
shows the same status live.

**Serial console** (115200) in receiver mode — handy when you don't want to
use the phone portal:

```
exit                  leave receiver mode, boot the player
info                  print status
cred <ssid> <pass> <token>   set credentials directly
```

Notes:
- Tokens and WiFi passwords are stored **only in the pod's NVS flash** — never
  in this repository.
- Files > 100 MB are refused; duplicate names get `_2`, `_3`… suffixes.
- Without an SD card the receiver reports `SD CARD MISSING!` and retries.
- The bot token is validated by Telegram: a bad one shows `token REFUSED`.

### SD card

FAT32. Put MP3s in `/music` (sub-folders up to 3 levels are scanned; if
`/music` is missing, the root is scanned).

- Best: **44.1 kHz, stereo, 128–256 kbps MP3**. Mono and 22/24/32/48 kHz
  files are resampled on the fly.
- Title/artist come from ID3v2.3/2.4 tags, otherwise from
  `Artist - Title.mp3` file names.
- Only ASCII is drawn (the built-in fonts have no Arabic/CJK glyphs); other
  characters display as `?`.

## Architecture

```
Playback:  SD (VSPI) -> Helix MP3 decoder (core 0 task) -> 16 KB PCM ring
           -> A2DP callback (BT task) -> headphones
UI:        LVGL on core 1 (main loop), 2 x 8-row draw buffers, TFT over HSPI
Touch:     XPT2046 bit-banged, own pins, 3-point calibration in NVS
Receiver:  softAP + WebServer portal (AP_STA) + Telegram getUpdates poller
           on a core-0 task; TLS via WiFiClientSecure (setInsecure)
```

**Why receiver mode exists as a separate boot:** the player boots with very
little heap to spare (LVGL + the Bluetooth stack are resident). TLS needs
tens of KB more. Rather than risk crashes, the WiFi/Telegram stack only runs
when LVGL and Bluetooth don't. The `wifi` NVS flag decides which mode boots;
both modes flip the flag and restart when switching.

**RAM notes** (the ESP32 has 320 KB DRAM, and BT + TLS + LVGL all want it):
draw buffers are 2 x 4 display rows, the BLE-only memory pools are released
at boot (`esp_bt_mem_release(ESP_BT_MODE_BLE)` — only classic BT/A2DP is
used), and the decoder task runs on a small stack. The SD library stores
track paths relative to `/music` (plus 2-byte offsets) to keep the whole
library table under ~3 KB. Settings → System shows live heap; keep an eye
on it if you add features.

## Troubleshooting

- **`SD: not found` at boot** — check the card is FAT32 and seated; the
  library will be empty and Telegram saves will fail.
- **Pod restarts every few seconds** — that's the Bluetooth connect running
  out of heap (fixed by the RAM trims above, but if you re-add big buffers
  watch the `[heap]` boot log lines). The BT connect is gated: it waits for
  ≥ 24 KB free before starting.
- **Colours inverted / garbled** — add `-D TFT_INVERSION_ON=1`, or swap
  `ILI9341_2_DRIVER` for `ILI9341_DRIVER` / `ST7789_DRIVER` (varies by CYD
  revision). Upside-down: `TFT_ROT 2` in `src/config.h`.
- **A2DP compile error on `a2dp.start()`** — library version differences:
  some want `a2dp.start("")`. If the pair list stays empty, set
  `DEFAULT_BT_NAME` in `config.h` to your earphones' exact name.
- **MP3 callback signature error** — older libhelix uses
  `(MP3FrameInfo&, short*, size_t)` without the trailing `void*`.
- **Auto-brightness backwards** — flip `LDR_DARK_IS_HIGH` in `config.h`.
- **Audio stutter** — use CPU 160/240 MHz; check free heap in
  Settings → System (BT + decoder + LVGL are tight).
- **Touch feels offset** — hold BOOT at power-up to recalibrate.

## Repository notes

- `.pio/` (build output, ~240 MB) and local logs are git-ignored.
- No secrets are committed: hotspot password, WiFi credentials and bot tokens
  are defaults or runtime NVS data on the device, not source content.

## Known limits / ideas

MP3 only (no FLAC/WAV/AAC yet) · VBR durations are estimates · seek is
byte-proportional · no real album art · no playlists · no battery gauge ·
receiver downloads are sequential (one file at a time).

## License

Released under the [MIT License](LICENSE).
