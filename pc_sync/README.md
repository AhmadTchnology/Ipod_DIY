# MyPod Sync (PC program)

Copies MP3 files, their tags and their **album covers** to the DIY iPod over the USB cable.

## Install (once)

1. Install Python 3.9 or newer (on Windows tick "Add Python to PATH"; on Linux also `sudo apt install python3-tk`).
2. In this folder: `pip install -r requirements.txt`
3. Windows only, if the pod does not show up as a COM port: install the CH340 or CP210x USB driver
   (the one used for flashing the board).

## Use

1. On the iPod: **Settings -> USB SYNC -> Copy songs from PC (USB)... -> Restart**. The screen shows "USB SYNC".
2. Run `python mypod_sync.py` (Windows: double-click `run_windows.bat`).
3. Pick the pod's port, press **Connect**. The window shows the card size and the songs already on it.
4. Add songs from your files (**Add files...** / **Add folder...**), import an
   **.m3u playlist** (see below), or from YouTube (next section),
   check the list (select a row to preview its cover), press **Send to iPod**.
5. When finished press **Finish & start the player**. The pod restarts and scans the card by itself.

## Playlists (.m3u)

**Import .m3u playlist...** on the *Songs to send* tab loads a playlist file
and queues every entry of it:

* **YouTube links** (a playlist exported from YouTube, a browser or a media player)
  are handled like anything from the YouTube tab: downloaded, converted, tagged with
  the cover art. Songs that are already in the local cache are reused as-is.
* **Local MP3 paths** (absolute or relative to the playlist file) are added directly.
* Entries that are neither (dead link, missing file) are skipped and listed in the log.
* The same song listed twice is queued only once.
* UTF-8 (Arabic, Russian, CJK...) and Windows-1252 playlists are both decoded correctly.
* A 121-entry playlist imports in about a second; nothing is downloaded until you
  press **Download now** or **Send to iPod**.

## From YouTube

The **YouTube** tab searches YouTube itself:

1. Type what you are looking for, press **Search** (or Enter). Results appear with
   thumbnails, the channel name and the duration; the "(Official Music Video)" and similar
   junk is cleaned from the song name automatically.
2. **Double-click a result** (or select several and press **Add selected to send list**,
   or use **Add all shown**) — they are queued in the *Songs to send* tab with a fresh name guess.
3. Press **Download now** (bottom bar) to fetch + convert them right away **without**
   needing the pod, or go straight to **Send to iPod**, which downloads first and then
   transfers (only the connected pod is needed for the send itself).

Details worth knowing:

* **Audio quality**: 192 kbps by default, 128-320 selectable - lower saves card space.
* **MP3 conversion needs ffmpeg.** The app looks for it in this order: your PATH, the
  `imageio-ffmpeg` pip package (installed as part of `requirements.txt`), or a specific
  location chosen with **Pick ffmpeg.exe...** (saved in `mypod_cfg.json`).
* Downloads land in `pc_sync/downloads/` - when you send the same song again it is reused
  and not downloaded twice. Deleting the folder is always safe.
* YouTube changes its site often: if downloads suddenly fail with errors like *403*,
  update the downloader:  `pip install -U yt-dlp`  (that already fixed it once during
  this program's development).
* Only the *audio* is used, the YouTube cover/thumbnail becomes the pod's 200x200 album art.

The *On the iPod* tab lists what is on the card and lets you delete songs (their covers are deleted too).

## What it does to your files

* The pod only plays **MP3**. Other formats are skipped (convert them with ffmpeg or Audacity first).
* The file is saved as `/music/Artist - Title.mp3` (taken from the tags; accents and other alphabets are
  converted to plain letters because the pod's font is ASCII only. `pip install unidecode` improves Arabic,
  Russian, Chinese ... titles).
* **Cover:** the embedded picture (JPEG, PNG, progressive, any size) is centre-cropped to a square, scaled to
  200x200 and stored in `/art/` on the card. By default the big picture is also removed from the *copy* that
  goes to the card (your original is never touched) - transfers are faster and the card stays small.
* Songs with the same name and size that are already on the card are skipped.

## Speed

Default 921600 baud (about 60-90 KB/s, a 5 MB song in roughly a minute). "1500000" / "2000000" are faster on
most boards. If the faster speed does not work the program falls back by itself.

## Troubleshooting

* *"The iPod did not answer"* - the pod is not in USB sync mode, or another program (serial monitor, flasher)
  has the port open. Close it and try again.
* Port missing - another USB cable (some are charge-only) or missing driver.
* The pod resets when you press Connect - normal on some boards; wait 3 seconds, it stays in USB sync mode.

## Developer test

`python tests/test_roundtrip.py` compiles the pod's real protocol code (`../src/usbsync_engine.h`) for the PC and
runs the transfer code against it (lost packets, corrupted frames, bad paths, covers, cancel ...). Needs `g++`.
