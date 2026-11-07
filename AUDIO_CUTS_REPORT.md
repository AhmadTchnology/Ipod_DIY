# MyPod — periodic audio cuts ("plays 1 s → stops ≈1 s → repeats") — profiling report

Device: Sunton CYD ESP32-2432S028 (ESP32 classic BT), 240 MHz, Arduino-ESP32 on ESP-IDF 4.4.x,
vendes ESP32-A2DP (pschatzmann) at commit ~7da8b6e + local patches, Helix MP3 decoder, ILI9341.
Firmware build under test: `Ipod_DIY_v1.1_USB_sync\Ipod_DIY_v1.1` (Telegram/WiFi code REMOVED in this
build — the A2DP link is the only radio running; no WiFi-BT coexistence).
Memory: RAM 51,044 B static, Flash 1,286,825 B. Library: 2 short tracks (a few seconds each).
Audio path: SD(VSPI 18/19/23/5) → reader ring (12 KB) → Helix decode (task "mp3dec", core 1, prio 2) →
6 KB PCM ring — wait — measured state /mp3buf=12 KB full / pcm ring 6144 B full →
A2DP data callback `a2dpData` (Bluedroid BTC task, core 0, SBC encode in-stack).

## Instrumentation on the device (this build)
- `[audio] ...` every 5 s while playing: underruns (ring short at BT callback), pcm ring fill,
  mp3 read-ahead fill, slowest decode µs, slowest SD read µs, decoder "waited for data" count.
- `[bt]` every 5 s: number of A2DP data callbacks, PCM bytes consumed from the ring by them,
  max gap between two consecutive callbacks (µs), callbacks arriving >20 ms after the previous one.
- `[bts]` on every Bluetooth connect/disconnect state change (200 ms poll).
- `[loop]` every 5 s: main-loop work per iteration, max iteration, iterations over 10 ms.
- `[player] NO AUDIO n s after open` — if a track was opened but g_frames is still 0 after 3 s.
- `[art] drawCover took Nns` — cover-paint duration when >25 ms.

## Main session (fresh boot → BT connect → playback; user listening)

```
[bts] 3840 ms: bt DISCONNECTED                      (start)
[clock] a2dp.start with 143976 ... [heap] ready: 51588 maxblock=36852
[bts] 15640 ms: bt CONNECTED                        (BT link up to the headphones/player device)
[player] open #0 heap free=26696 readahead=1
[art] drawCover took 88557us                        (88 ms of blocking cover paint on track change)
[audio] underruns=0(+0) pcm=6144/6144 mp3buf=12288/12288 | slowest decode=12 ms, slowest SD read=7 ms, waited +5 | heap=21860
[bt]  calls=0     bytes=0        maxGap=0us      over20ms=0        (window where media not yet STARTED)
[loop] work=9us max=216425us over10ms=4            (loop blocked 216 ms during a connect burst)
[player] open #1 heap free=21268 readahead=1        (auto-advance to track #1)
[art] drawCover took 160650us                       (161 ms cover paint!)
[player] NO AUDIO 3s after open: bt=1 eof=0 prebuf=1 pcm=6144/6144 readahead=1 rdRun=1 rdEof=0 mp3buf=12288 starves=9 heap=21284
[bt]  calls=0     bytes=0        maxGap=0us      over20ms=0        (5 s with ZERO BT callbacks while bt=1)
[player] open #0 heap free=21284 readahead=1        (user pressed next)
[art] drawCover took 170750us                       (171 ms cover paint)
[bt]  calls=266   bytes=132608  maxGap=10706960us over20ms=38      (10.71-SECOND no-data hole before this)
[bt]  calls=1169  bytes=598528  maxGap=27859us   over20ms=167
[bt]  calls=1169  bytes=598528  maxGap=27868us   over20ms=167
[bt]  calls=1169  bytes=598528  maxGap=27877us   over20ms=167      (steady-state windows, see math below)
[bt]  calls=1164  bytes=595968  maxGap=27874us   over20ms=167
[bt]  calls=1172  bytes=600064  maxGap=27909us   over20ms=168
... (several more windows, all ~1169/598K/27.9 ms/167)
```

## Derived measurements (the important ones)

1. **Steady-state PCM delivery rate is only ~68 % of real time.**
   Windows of 5.000 s deliver 598,528 bytes PCM = 149,632 stereo samples = 3.39 s of 44.1 kHz audio.
   i.e. the BTC pump asks for 512 byte/call at 233.8 calls/s → 119,705 B/s against the 176,400 B/s needed.
   The sink device plays at 100 % real time → it must run dry every ~1.5 s and re-buffer — this is exactly
   the audible "plays for a second, stops, plays again" cycle.
2. **A full 10.71 s hole with zero callbacks and zero warnings**, while `bt=1` (AVDTP connection!) and
   everything local is full. `[player] NO AUDIO 3s after open ... prebuf=1 mp3buf=12288` — the prebuf gate
   cannot clear because nothing drains it (the gate is evaluated inside the stopped callback).
3. **Persistent per-callback scheduling jitter on core 0:** in every 5-s window, ~167 of ~1169 callbacks
   (14 %) arrive >20 ms after the previous one; maxGap sits at 27.8-27.9 ms. Average interval is 4.28 ms
   (233.8/s), so roughly every 7th callback slot a ~25-28 ms stall occurs, forever.
4. **Local audio pipeline never starves:** pcm ring full at 6144/6144 in every snapshot,
   `underruns=0` through the whole session, decode 9-12 ms/frame (max seen), SD reads 6-8 ms.
   The `[audio] ring stays 6144B` guard refuses to grow the ring because heap leaves only ~17-26 KB
   free with maxblock 12-14 KB (Bluetooth owns the rest).
5. **UI thread hiccups:** `[loop]` max = 66.3 ms, 141-226 ms (around the connect burst /art draws),
   ~1-12 iterations >10 ms per window; `[art]` draws 88-171 ms each track change (loop is on the same
   core as mp3dec, prio 1 < 2, so the decoder pre-empts it; it matters only for UI smoothness).

## What has been ruled out
- The MP3/decoder/SD path (it stays saturated; full pipeline ahead of Bluetooth).
- Local ring underruns feeding the SBC encoder (counter never increments).
- WiFi/BT radio coexistence (no WiFi stack in this build) + no I2S/SD/touch on the BT pins.
- Heap exhaustion during playback (min free 12.9-17.1 KB, maxblock 12.2-14.3 KB, no events logged).

## Remaining hypotheses, ranked

A. **Sink-side radio throughput limit (strong favorite).** Callback pacing exactly matches a rate-limited
   eSCO… eACL link: 233.8 pkt-eq/s with repeating single-slot stalls every ~30 ms = real rate of ~120 kB/s
   on air vs a healthy link's ~200-300 kB/s. If the radio link is degraded (distance, body blocking the
   antenna at GPIO/PCB antenna, 2.4 GHz interference steering from a Wi-Fi router next to the pod,
   or a weak headphone), Bluedroid throttles sends to what the link acks and the PCM requests slow
   to the observed 68 %. A 68 %-Fed sink starves cyclically ≤1-2 s: the reported symptom.

   Test discriminators: (1) move headphones ~20-30 cm from the pod (avoid touching the antenna area
   of the PCB!) → if the rate jumps to ~100 % (176,400 B/s) the stutter ends; (2) power the pod's
   2.4-GHz environment: your router "Ahmed Light Speed" on channel 6/11 — try moving away from it;
   (3) test on a different BT sink (another speaker/headset).

B. **Media engine pump mis-pacing in the stack** (BTC housekeeping every 30 ms with the *internal*
   ESP-IDF SBC encoder). The vendored lib was patched (AVRCP CT disabled, heartbeat timer guard)
   to protect heap; but the legacy source data path asks 512 B per invocation from Bluedroid BTC.
   If the BTC task's 68 % rate comes from *its own* pacing rather than the radio, then the
   sink is healthy and the pump needs mitigation (e.g., bigger queue depths in sdkconfig
   `CONFIG_BT_A2DP_*`, SBC bitrate/frames-per-packet, or the managed encoder which won't compile
   on IDF 4.4).

   Test discriminator: use the SAME pod with another phone/sink and verify — if a different sink
   is clean, B is disproven and A (or the particular sink) is confirmed; if ALL sinks stutter at
   ~68 %, then it is B (the pod's own pump/stack, not the radio).

C. **Sink firmware behavior:** some BT devices (esp. phones in developer mode/battery saver or
   cheap mono kits) suspend/pause the AVDTP media stream periodically and rely on the source to
   resume quickly; suspends leave the link up (bt=1) with the observed zero-callback hole.
   The 10.7 s hole and the "NO AUDIO, prebuf=1, mp3buf full, bt=1" state are consistent with a
   *sink-initiated suspend* the source did not resume until the user interacted again.
   Also AVRCP CT is disabled in the vendored lib (source-only patch): if the sink expects AVRCP
   play/pause signalling in both directions this can contribute — try re-enabling
   `esp_avrc_ct_init/register` and see if the periodic hole disappears.

## Not the cause (but worth fixing anyway)
- `[art] drawCover` at 88-171 ms on track changes blocking the loop task. If it ever runs while the
  A2DP callback needs the SD… it reads via the shared sdMx — with priority inheritance the BT task is
  unaffected (art never holds it >171 ms total, in 4-row slices), but it is ugly on track changes.
- `[loop] max=226 ms` connect-burst stalls: cosmetic on screen; audio unaffected (pinned survey).

## Raw captures
Complete serial transcripts of the sessions described above are in the chat transcript of this same
date; the instrumentation is present in the source tree (`src/player.cpp`, `src/main.cpp`, `src/art.cpp`)
so any further session reproduces the same counters.
