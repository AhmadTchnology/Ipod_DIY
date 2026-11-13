"""End-to-end test: the real C++ device engine (host build) talking to the real Python protocol code.
Run:  python tests/test_roundtrip.py      (needs g++, pyserial not required)"""
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import mypod_proto as P
import mypod_media as M


class PipeTransport:
    """select.select() can't wait on pipes on Windows, so a daemon thread keeps the
    child's stdout drained and read() waits on the buffer instead (works on all OSes)."""
    def __init__(self, proc):
        self.p = proc
        self.baud = 115200
        self._buf = bytearray()
        self._cv = threading.Condition()
        self._eof = False
        threading.Thread(target=self._pump, daemon=True).start()
    def _pump(self):
        fd = self.p.stdout.fileno()
        while True:
            try:
                data = os.read(fd, 65536)
            except OSError:
                data = b""
            with self._cv:
                self._buf.extend(data)
                self._eof = not data
                self._cv.notify_all()
                if not data:
                    return
    def write(self, data):
        self.p.stdin.write(data); self.p.stdin.flush()
    def read(self, timeout):
        deadline = time.time() + timeout
        with self._cv:
            while not self._buf and not self._eof:
                left = deadline - time.time()
                if left <= 0:
                    return b""
                self._cv.wait(min(left, 0.05))
            out = bytes(self._buf)
            self._buf.clear()
            return out
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
    check(M.device_name("Beyoncé", "Halo (Live)", "x") == "Beyoncé - Halo (Live)", "accents kept (pod renders Unicode now)")
    check(M.device_name("", "", "my?bad:file*name") == "my bad file name", "illegal characters removed")
    check(M.device_name("", "", "***") == "Track", "empty name falls back")
    check(len(M.device_name("a" * 200, "b" * 200, "x")) <= 143, "long names are bounded")
    check(M.device_name("حسام الرسام", "متكبره", "x") == "حسام الرسام - متكبره", "Arabic artist/title kept")
    check(M.device_name("", "", "a\u0001b") == "a b", "control characters removed")
    check(len(M.device_name("ح" * 100, "", "x").encode("utf-8")) <= 70, "byte cap holds for Arabic")
    check(M.device_name("", "", "攬佬SKAI ISYOURGOD") == "攬佬SKAI ISYOURGOD", "CJK kept")

    # --- Unicode filenames end to end: the pod stores and hashes UTF-8 bytes
    uni_song = "/music/حسام الرسام - متكبره.mp3"
    dev.put(uni_song, b"u" * 5000)
    check(open(os.path.join(root, "music", "حسام الرسام - متكبره.mp3"), "rb").read() == b"u" * 5000,
          "Arabic-named song stored byte-exact")
    check(any(s["path"] == uni_song for s in dev.list_songs()), "LIST returns the Arabic name as UTF-8")
    ucp = M.cover_dev_path(uni_song)
    srcu = ('#include "../../src/arthash.h"\n#include <stdio.h>\nint main(){char o[40];char p[]=' +
            "".join('"\\x%02x"' % b for b in uni_song.encode("utf-8")) +
            ';artCachePath(p,o,40);puts(o);}')
    spu = os.path.join(tempfile.gettempdir(), "hu.cpp"); open(spu, "w").write(srcu)
    subprocess.run(["g++", "-I", HERE, "-x", "c++", "-o", spu + ".x", spu], check=True, capture_output=True)
    outu = subprocess.run([spu + ".x"], capture_output=True, text=True).stdout.strip()
    check(outu == ucp, f"PC and device agree on the UTF-8 cover file name ({outu})")
    dev.put(ucp, blob)
    dev.delete(uni_song)
    check(not os.path.exists(os.path.join(root, "music", "حسام الرسام - متكبره.mp3")), "Arabic-named song deleted")
    try:
        dev.put("/music/bad?name.mp3", b"x"); ok = False
    except P.DeviceError:
        ok = True
    check(ok, "'?' still refused")

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

    # --- cover quality: precise rounding, dithering, colour boost, force_covers
    import io as _io
    from PIL import Image as _PImg, ImageEnhance as _Enh

    def _mean_sat_img(im):
        tot = 0.0
        for r, g, b in im.getdata():
            mx, mn = max(r, g, b), min(r, g, b)
            tot += 0.0 if mx == 0 else (mx - mn) / mx
        return tot / 40000

    white = _PImg.new("RGB", (400, 400), (255, 255, 255))
    wb = _io.BytesIO(); white.save(wb, "PNG")
    bw = M.make_cover_blob(wb.getvalue(), 0, saturation=1.0, sharpen=False)
    v0 = struct.unpack("<H", bw[16:18])[0]
    check(v0 == 65535, f"pure white rounds to 0xFFFF (got {v0:#06x})")

    strip = _PImg.new("L", (400, 1))
    for x in range(400):
        strip.putpixel((x, 0), (x * 255) // 399)
    ramp = strip.resize((400, 400), _PImg.NEAREST).convert("RGB")
    rb = _io.BytesIO(); ramp.save(rb, "PNG")

    def _ramp_err(blob):
        px = struct.unpack("<40000H", blob[16:])
        err = 0.0
        for by in range(50):
            for bx in range(50):
                ideal = sum((8 * bx + xx) * 255 / 399.0 for xx in range(8)) / 8
                got = 0.0
                for yy in range(4):
                    row = (4 * by + yy) * 200
                    for xx in range(4):
                        got += ((px[row + 4 * bx + xx] >> 5) & 63) * 255 / 63
                err += abs(got / 16 - ideal)
        return err
    ed = _ramp_err(M.make_cover_blob(rb.getvalue(), 0, saturation=1.0, sharpen=False, dither=True))
    en = _ramp_err(M.make_cover_blob(rb.getvalue(), 0, saturation=1.0, sharpen=False, dither=False))
    check(ed < en, f"dithered grey ramp reconstructs closer to the true ramp ({ed:.2f} < {en:.2f})")

    muted = _PImg.new("RGB", (400, 400), (170, 110, 110))
    mb = _io.BytesIO(); muted.save(mb, "PNG")
    base_im = _PImg.open(_io.BytesIO(mb.getvalue())).convert("RGB")
    boost_im = _Enh.Color(base_im).enhance(1.25)
    s0, s125 = _mean_sat_img(base_im), _mean_sat_img(boost_im)
    check(s125 > 1.15 * s0,
          f"colour boost 1.25 raises saturation >15% ({s125:.3f} vs {s0:.3f})")
    b0 = _mean_sat_img(M.blob_to_image(M.make_cover_blob(mb.getvalue(), 0, saturation=1.0, sharpen=False, dither=False)))
    b1 = _mean_sat_img(M.blob_to_image(M.make_cover_blob(mb.getvalue(), 0, saturation=1.25, sharpen=False, dither=False)))
    check(b1 > b0,
          f"boosted cover stays more saturated through the RGB565 blob ({b1:.3f} > {b0:.3f})")

    cov = os.path.join(root, M.cover_dev_path("/music/Band - Song.mp3").lstrip("/"))
    cur_size = next(s["size"] for s in dev.list_songs() if s["path"] == "/music/Band - Song.mp3")
    work2 = tempfile.mkdtemp(prefix="mypod_cov_")            # the earlier work dir was already cleaned up
    ta_re = {"path": os.path.join(work2, "a.mp3"), "name": "Band - Song",
             "info": {"cover": jb.getvalue()}}               # same name + size as the one on the card
    open(ta_re["path"], "wb").write(b"A" * cur_size)
    mtime0 = os.path.getmtime(cov)
    r4 = B.run_batch(dev, [(0, ta_re)], False, True, lambda: False,
                     lambda i, s: None, lambda *a: None, print)
    check(r4 == (0, 1, 0), f"re-run without force_covers still skips the song: {r4}")
    check(os.path.getmtime(cov) == mtime0, "existing cover file untouched without force_covers")
    diff = _PImg.new("RGB", (300, 300), (10, 180, 60))
    db = _io.BytesIO(); diff.save(db, "PNG")
    taf = dict(ta_re, info={"cover": db.getvalue()})
    before = open(cov, "rb").read()
    r5 = B.run_batch(dev, [(0, taf)], False, True, lambda: False,
                     lambda i, s: None, lambda *a: None, print, force_covers=True)
    check(r5 == (0, 1, 0) and open(cov, "rb").read() != before,
          f"force_covers skips the song, replaces only the cover: {r5}")
    shutil.rmtree(work2, ignore_errors=True)

    # --- near-white mush (typical JPEG artefact) snaps to clean white
    pale = _PImg.new("RGB", (400, 400), (250, 250, 251))
    nb = _io.BytesIO(); pale.save(nb, "PNG")
    v1 = struct.unpack("<H", M.make_cover_blob(nb.getvalue(), 0, saturation=1.0, sharpen=False)[16:18])[0]
    check(v1 == 65535, f"near-white JPEG background snaps to clean white (got {v1:#06x})")

    # --- deleting a song that is not there must refuse, not ack a lie
    try:
        dev.delete("/music/no such song.mp3"); ok = False
    except P.DeviceError as e:
        ok = e.code == 2                                   # E_PATH
    check(ok, "deleting a missing song reports an error instead of success")

    # --- exit
    dev.exit_to_player()
    time.sleep(0.2)
    check(proc.poll() is not None, "device engine left sync mode on EXIT")
    shutil.rmtree(root, ignore_errors=True)
    print("ALL OK" if not check.fails else f"{check.fails} FAILED")
    sys.exit(1 if check.fails else 0)


if __name__ == "__main__":
    main()
