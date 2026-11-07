# Bluetooth fix for the "plays 1 s, stops, plays again" cuts

Based on AUDIO_CUTS_REPORT.md. Nothing in your `player.cpp` / `main.cpp` / `art.cpp` is overwritten.

## Install (3 steps)
1. Copy `lib/ESP32-A2DP/src/BluetoothA2DPSource.cpp` over the one in your project (same path). `library.patch` shows the 3 small edits.
2. Copy `src/btfix.h` and `src/btfix.cpp` into your `src/`.
3. In your `src/player.cpp` add two lines:
   * near the other includes:  `#include "btfix.h"`
   * first line inside `void update() {` (before `maybeEnableReadAhead();`):
     `BtFix::tick(a2dp, g_loaded && !g_paused);`

## What the report showed, and what this changes
| Finding in the report | Cause found in the code | Change |
|---|---|---|
| 10.7 s with zero BT callbacks, `bt=1`, `NO AUDIO ... prebuf=1` | After the headphones connect, the library never sends "start streaming"; it waits for its 10 s heartbeat timer (`BT_APP_HEART_BEAT_EVT`), exactly the 10.7 s hole | Library now sends `CHECK_SRC_RDY` the moment the link is up (what the ESP-IDF example does) |
| Silent until the next user action | If the sink suspends/stops the stream the library still believes it is STARTED and never restarts it | Library resets its state and restarts (max once a second); `BtFix` also re-asks after 2.5 s of "no stream" while a track is playing |
| `get_audio_state()` unusable | The library only stored the audio state when a user callback was registered | State is always stored now |
| Stack pumps ~68 % of real time (~120 KB/s of 176 KB/s) | Pump back-pressure from the radio link (hypothesis A in the report). Not a CPU/SD/decoder problem: the report proves the local pipeline is full | `BtFix` sets BR/EDR TX power to the maximum (+9 dBm) once the controller is up, so the sink needs fewer retransmissions |

The 68 % pump is the main cause of the audible cycle and it is a link-throughput limit, not something the
player code can speed up. Higher TX power is the one software lever; if the new boot log line
`[bt] BR/EDR tx power set -> ok (... max=7 ...)` appears and `[bt] calls=` per 5 s still stays near 1170:

* move the headphones 20-30 cm closer / keep a hand off the antenna area of the CYD, and keep the pod away from the router
* try a second Bluetooth sink - if that one reaches ~1720 calls per 5 s, the first sink/link is the limit
* if every sink stays at ~1170, send me the `[bt]`/`[bts]` lines again; the next step would be rebuilding with a custom sdkconfig

Expected in the log after this patch: `[bt] BR/EDR tx power set -> ok`, media starting right after `[bts] ... CONNECTED`
(no more 10 s wait), and `calls` per 5 s should rise from ~1170 towards ~1720 if the link was the limit.
