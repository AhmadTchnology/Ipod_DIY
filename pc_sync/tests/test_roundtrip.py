"""End-to-end test: the real C++ device engine (host build) talking to the real Python protocol code.
Run:  python tests/test_roundtrip.py      (needs g++, pyserial not required)"""
import os
import random
import select
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import mypod_proto as P
import mypod_media as M


class PipeTransport:
    def __init__(self, proc):
        self.p = proc
        self.baud = 115200
    def write(self, data):
        self.p.stdin.write(data); self.p.stdin.flush()
    def read(self, timeout):
        r, _, _ = select.select([self.p.stdout], [], [], timeout)
        return os.read(self.p.stdout.fileno(), 65536) if r else b""
    def set_baud(self, b):
        self.baud = b
    def flush_input(self):
        pass


def build():
    exe = os.path.join(tempfile.gettempdir(), "mypod_host_device")
    cmd = ["g++", "-std=c++17", "-O1", "-Wall", "-Wno-unused-function", "-I", HERE, "-o", exe,
           os.path.join(HERE, "host_device.cpp")]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(r.stderr); sys.exit("host build failed")
    return exe


def check(cond, msg):
    print(("PASS " if cond else "FAIL ") + msg)
    if not cond:
        check.fails += 1
check.fails = 0


def main():
    exe = build()
    root = tempfile.mkdtemp(prefix="mypod_sd_")
    proc = subprocess.Popen([exe, root], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    dev = P.Device(PipeTransport(proc))

    # garbage first (a real pod prints boot noise) then hello
    proc.stdin.write(b"ets Jun  8 2016 00:22:57\r\nrst:0x1 (POWERON_RESET)\xA5\x00\xFF" * 3); proc.stdin.flush()
    h = dev.hello()
    check(h["sd"] and h["name"] == "MyPod" and h["proto"] == 1, f"hello after boot garbage -> {h}")
    total, free = dev.free_space()
    check(total > free > 0, "free space reported")

    # --- song upload with a 'real-ish' mp3 body
    rnd = random.Random(1)
    song = bytes(rnd.getrandbits(8) for _ in range(300_001))
    seen = []
    dev.put("/music/Daft Punk - Get Lucky.mp3", song, progress=lambda d, t: seen.append(d))
    got = open(os.path.join(root, "music/Daft Punk - Get Lucky.mp3"), "rb").read()
    check(got == song, f"300001-byte song arrives bit-exact ({len(seen)} chunks)")
    check(not os.path.exists(os.path.join(root, "music/Daft Punk - Get Lucky.mp3.tmp")), "no .tmp left behind")
    check(os.path.exists(os.path.join(root, "libdirty.txt")), "library flagged for re-scan")

    # --- subfolder + overwrite
    dev.put("/music/Sub/Folder/x.mp3", b"abc" * 1000)
    dev.put("/music/Sub/Folder/x.mp3", b"zz")
    check(open(os.path.join(root, "music/Sub/Folder/x.mp3"), "rb").read() == b"zz", "nested folders created, overwrite works")

    # --- cover: PC renders it, device stores it under the hash the player will look up
    from PIL import Image
    import io
    img = Image.new("RGB", (640, 480))
    for x in range(640):
        for y in range(0, 480, 8):
            img.putpixel((x, y), (x % 256, y % 256, 128))
    jb = io.BytesIO(); img.save(jb, "JPEG"); 
    blob = M.make_cover_blob(jb.getvalue(), len(song))
    check(len(blob) == 16 + 200 * 200 * 2 and blob[:4] == b"MCOV", f"cover blob size {len(blob)}")
    cp = M.cover_dev_path("/music/Daft Punk - Get Lucky.mp3")
    dev.put(cp, blob)
    check(open(os.path.join(root, cp.lstrip("/")), "rb").read() == blob, f"cover stored at {cp}")
    # same FNV the C++ uses (arthash.h): compile a tiny check
    src = '#include "../../src/arthash.h"\nint main(int c,char**v){char o[40];artCachePath(v[1],o,40);puts(o);}'
    sp = os.path.join(tempfile.gettempdir(), "h.cpp"); open(sp, "w").write(src)
    subprocess.run(["g++", "-I", HERE, "-x", "c++", "-o", sp + ".x", sp], check=True, capture_output=True)
    out = subprocess.run([sp + ".x", "/music/Daft Punk - Get Lucky.mp3"], capture_output=True, text=True).stdout.strip()
    check(out == cp, f"PC and device agree on the cover file name ({out})")

    # --- list
    songs = dev.list_songs()
    byp = {s["path"]: s for s in songs}
    check(len(songs) == 2 and byp["/music/Daft Punk - Get Lucky.mp3"]["cover"] and not byp["/music/Sub/Folder/x.mp3"]["cover"]
          and byp["/music/Daft Punk - Get Lucky.mp3"]["size"] == len(song), f"list shows both songs + cover flags: {[(s['path'], s['size'], s['cover']) for s in songs]}")

    # --- rejects
    for bad in ["/etc/passwd.mp3", "/music/../x.mp3", "/music/a.txt", "/music/a\\b.mp3", "/art/x.mp3"]:
        try:
            dev.put(bad, b"x"); ok = False
        except P.DeviceError:
            ok = True
        check(ok, f"path refused: {bad}")

    # --- corrupted CRC is caught and the half file is removed
    orig = zlib.crc32
    try:
        P.zlib = type("Z", (), {"crc32": staticmethod(lambda d, c=0: (orig(d, c) ^ 1))})
        try:
            dev.put("/music/bad.mp3", b"q" * 5000); ok = False
        except P.DeviceError as e:
            ok = e.code == 7
    finally:
        P.zlib = zlib
    check(ok and not os.path.exists(os.path.join(root, "music/bad.mp3")), "crc mismatch rejected, nothing saved")

    # --- lost ACK -> PC resends the same chunk, device must not write it twice
    data = bytes(range(256)) * 40
    real_next = dev._next_frame
    state = {"dropped": False}
    def lossy(timeout):
        fr = real_next(timeout)
        if fr[0] == (P.CMD_PUT_DATA | 0x80) and not state["dropped"]:
            state["dropped"] = True
            return real_next(timeout) if False else (_ for _ in ()).throw(P.TimeoutError_("simulated lost ack"))
        return fr
    dev._next_frame = lossy
    dev.put("/music/lossy.mp3", data)
    dev._next_frame = real_next
    check(open(os.path.join(root, "music/lossy.mp3"), "rb").read() == data and state["dropped"], "lost ACK recovered without duplicate data")

    # --- corrupted frame bytes on the wire are ignored and retried
    t = dev.t
    real_write = t.write
    cnt = {"n": 0}
    def flaky(d):
        cnt["n"] += 1
        if cnt["n"] == 3:
            d = bytearray(d); d[len(d) // 2] ^= 0xFF; d = bytes(d)
        real_write(d)
    t.write = flaky
    dev.put("/music/flaky.mp3", b"w" * 20000)
    t.write = real_write
    check(open(os.path.join(root, "music/flaky.mp3"), "rb").read() == b"w" * 20000, "bit-flipped frame -> retry -> intact file")

    # --- delete removes the song and its cover
    dev.delete("/music/Daft Punk - Get Lucky.mp3")
    check(not os.path.exists(os.path.join(root, "music/Daft Punk - Get Lucky.mp3")) and
          not os.path.exists(os.path.join(root, cp.lstrip("/"))), "delete removes song + cover")

    # --- baud negotiation (host shim just records it)
    check(dev.set_baud(921600) and open(os.path.join(root, "baud.txt")).read() == "921600", "baud change accepted")
    try:
        dev.request(P.CMD_SETBAUD, struct.pack("<I", 12345)); ok = False
    except P.DeviceError as e:
        ok = e.code == 9
    check(ok, "odd baud rates refused")

    # --- names
    check(M.device_name("Beyoncé", "Halo (Live)", "x") == "Beyonce - Halo (Live)", "accents transliterated")
    check(M.device_name("", "", "my?bad:file*name") == "my bad file name", "illegal characters removed")
    check(M.device_name("", "", "***") == "Track", "empty name falls back")
    check(len(M.device_name("a" * 200, "b" * 200, "x")) <= 143, "long names are bounded")

    # --- mp3 tag + cover extraction and strip (needs mutagen)
    try:
        from mutagen.id3 import ID3, TIT2, TPE1, TALB, APIC
        mp3 = os.path.join(root, "t.mp3")
        # minimal valid MPEG1 L3 128k frames so mutagen accepts it
        frame = b"\xFF\xFB\x90\x00" + b"\x00" * 413
        open(mp3, "wb").write(frame * 50)
        tg = ID3(); tg.add(TIT2(encoding=3, text="Song")); tg.add(TPE1(encoding=3, text="Band")); tg.add(TALB(encoding=3, text="Disc"))
        tg.add(APIC(encoding=3, mime="image/jpeg", type=3, desc="c", data=jb.getvalue())); tg.save(mp3, v2_version=3)
        info = M.read_info(mp3)
        check((info["title"], info["artist"], info["album"]) == ("Song", "Band", "Disc") and info["cover"] == jb.getvalue(), f"tags + cover read ({info['seconds']}s)")
        sp2, is_tmp = M.stripped_copy(mp3)
        i2 = M.read_info(sp2)
        check(is_tmp and i2["cover"] is None and i2["title"] == "Song" and os.path.getsize(sp2) < os.path.getsize(mp3), "strip removes the picture, keeps tags")
        if is_tmp: os.remove(sp2)
    except ImportError:
        print("SKIP mutagen tests (pip install mutagen)")

    # --- the same batch code the GUI uses: dedupe names, skip duplicates, covers, replace
    import mypod_batch as B
    work = tempfile.mkdtemp(prefix="mypod_src_")
    def mk(name, body):
        q = os.path.join(work, name); open(q, "wb").write(body); return q
    ta = {"path": mk("a.mp3", b"A" * 9000), "name": "Band - Song", "info": {"cover": jb.getvalue()}}
    tb = {"path": mk("b.mp3", b"B" * 7000), "name": "Band - Song", "info": {}}        # same tags, other file
    tc = {"path": mk("c.mp3", b"C" * 100), "name": "Solo - Tune", "info": {"cover": b"not an image"}}
    rows = []
    r = B.run_batch(dev, list(enumerate([ta, tb, tc])), False, True, lambda: False,
                    lambda i, s: rows.append((i, s)), lambda *a: None, print)
    mp = os.path.join(root, "music")
    check(r == (3, 0, 0), f"batch sent 3 songs: {r}")
    check(open(os.path.join(mp, "Band - Song.mp3"), "rb").read() == b"A" * 9000 and
          open(os.path.join(mp, "Band - Song (2).mp3"), "rb").read() == b"B" * 7000, "same-name songs both kept")
    check(os.path.exists(os.path.join(root, M.cover_dev_path("/music/Band - Song.mp3").lstrip("/"))), "cover sent with the song")
    check(not os.path.exists(os.path.join(root, M.cover_dev_path("/music/Solo - Tune.mp3").lstrip("/"))), "unreadable cover skipped, song still sent")
    r2 = B.run_batch(dev, list(enumerate([ta, tb, tc])), False, True, lambda: False,
                     lambda i, s: None, lambda *a: None, print)
    check(r2 == (0, 3, 0), f"second run skips everything already there: {r2}")
    ta2 = dict(ta, path=mk("a2.mp3", b"A" * 9500))
    r3 = B.run_batch(dev, [(0, ta2)], False, True, lambda: False, lambda i, s: None, lambda *a: None, print)
    check(r3 == (1, 0, 0) and open(os.path.join(mp, "Band - Song.mp3"), "rb").read() == b"A" * 9500, "changed file replaces the old version")
    cancelled = {"n": 0}
    def cancel_after(): cancelled["n"] += 1; return cancelled["n"] > 6
    try:
        B.run_batch(dev, [(0, dict(ta, path=mk("big.mp3", b"Z" * 200000), name="Big One"))], False, True, cancel_after, lambda i, s: None, lambda *a: None, print)
        ok = False
    except P.Cancelled:
        ok = True
    check(ok and not os.path.exists(os.path.join(mp, "Big One.mp3")) and not os.path.exists(os.path.join(mp, "Big One.mp3.tmp")), "cancel aborts cleanly, no partial file")
    shutil.rmtree(work, ignore_errors=True)

    # --- exit
    dev.exit_to_player()
    time.sleep(0.2)
    check(proc.poll() is not None, "device engine left sync mode on EXIT")
    shutil.rmtree(root, ignore_errors=True)
    print("ALL OK" if not check.fails else f"{check.fails} FAILED")
    sys.exit(1 if check.fails else 0)


if __name__ == "__main__":
    main()
