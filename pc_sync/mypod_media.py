"""Reads MP3 tags, builds the iPod-friendly file name and the 200x200 RGB565 cover."""
import array
import io
import os
import re
import shutil
import struct
import sys
import tempfile
import unicodedata

COVER_PX = 200
try:
    from unidecode import unidecode as _unidecode
except Exception:            # optional: romanises Arabic/Cyrillic/CJK for the iPod's ASCII font
    _unidecode = None


def fnv1a(path_bytes):
    h = 2166136261
    for b in path_bytes:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def cover_dev_path(song_dev_path):
    return "/art/%08x.cov" % fnv1a(song_dev_path.encode("ascii"))


def to_ascii(s):
    if not s:
        return ""
    if _unidecode:
        return _unidecode(s)
    return unicodedata.normalize("NFKD", s).encode("ascii", "ignore").decode("ascii")


_BAD = re.compile(r"[^A-Za-z0-9 _.,'()&+!#%@$=~\[\]-]")


def clean_name(s, limit=70):
    s = _BAD.sub(" ", to_ascii(s))
    s = re.sub(r"\s+", " ", s).strip(" .")
    return s[:limit].strip(" .")


def device_name(artist, title, stem):
    a, t = clean_name(artist), clean_name(title)
    if a and t:
        name = f"{a} - {t}"
    else:
        name = t or clean_name(stem)
    if sum(c.isalnum() for c in name) < 2:
        name = clean_name(stem) or "Track"
        if sum(c.isalnum() for c in name) < 2:
            name = "Track"
    return name


def read_info(path):
    """-> dict(title, artist, album, seconds, bitrate_kbps, cover_bytes or None)"""
    info = {"title": "", "artist": "", "album": "", "seconds": 0, "bitrate": 0, "cover": None}
    try:
        from mutagen.mp3 import MP3
        from mutagen.id3 import ID3, ID3NoHeaderError
        audio = MP3(path)
        info["seconds"] = int(audio.info.length)
        info["bitrate"] = int(audio.info.bitrate // 1000)
        try:
            tags = ID3(path)
        except ID3NoHeaderError:
            tags = None
        if tags:
            def txt(key):
                fr = tags.get(key)
                return str(fr.text[0]).strip() if fr and fr.text else ""
            info["title"], info["artist"], info["album"] = txt("TIT2"), txt("TPE1") or txt("TPE2"), txt("TALB")
            pics = tags.getall("APIC")
            if pics:
                best = next((p for p in pics if p.type == 3), pics[0])
                info["cover"] = bytes(best.data)
    except Exception as e:
        info["error"] = str(e)
    return info


def _to_srgb(im):
    """Honour an embedded colour profile: Adobe RGB / Display P3 / CMYK covers look
    washed out if their ICC profile is ignored. Falls back to a plain RGB convert."""
    try:
        if im.info.get("icc_profile"):
            from PIL import ImageCms
            src = ImageCms.ImageCmsProfile(io.BytesIO(im.info["icc_profile"]))
            im = ImageCms.profileToProfile(im, src, ImageCms.createProfile("sRGB"),
                                           outputMode="RGB")
        return im.convert("RGB")
    except Exception:
        return im.convert("RGB")


def _rgb565_dithered(raw, w, h):
    out = [0] * (w * h)
    rl = [i * 255 / 31.0 for i in range(32)]
    gl = [i * 255 / 63.0 for i in range(64)]
    errR = [0.0] * (w + 2)
    errG = [0.0] * (w + 2)
    errB = [0.0] * (w + 2)
    for y in range(h):
        nR = [0.0] * (w + 2)
        nG = [0.0] * (w + 2)
        nB = [0.0] * (w + 2)
        base = y * w * 3
        rng = range(w) if y % 2 == 0 else range(w - 1, -1, -1)
        d = 1 if y % 2 == 0 else -1
        for x in rng:
            i = base + x * 3
            r = raw[i] + errR[x + 1]
            g = raw[i + 1] + errG[x + 1]
            b = raw[i + 2] + errB[x + 1]
            qr = int(r * 31 / 255 + 0.5); qr = 0 if qr < 0 else (31 if qr > 31 else qr)
            qg = int(g * 63 / 255 + 0.5); qg = 0 if qg < 0 else (63 if qg > 63 else qg)
            qb = int(b * 31 / 255 + 0.5); qb = 0 if qb < 0 else (31 if qb > 31 else qb)
            er, eg, eb = r - rl[qr], g - gl[qg], b - rl[qb]
            xn = x + 1 + d
            errR[xn] += er * 0.4375; errG[xn] += eg * 0.4375; errB[xn] += eb * 0.4375
            nR[x + 1 - d] += er * 0.1875; nG[x + 1 - d] += eg * 0.1875; nB[x + 1 - d] += eb * 0.1875
            nR[x + 1] += er * 0.3125; nG[x + 1] += eg * 0.3125; nB[x + 1] += eb * 0.3125
            nR[xn] += er * 0.0625; nG[xn] += eg * 0.0625; nB[xn] += eb * 0.0625
            out[y * w + x] = (qr << 11) | (qg << 5) | qb
        errR, errG, errB = nR, nG, nB
    return out


def make_cover_blob(cover_bytes, mp3_size, saturation=1.25, sharpen=True, dither=True):
    """Any JPEG/PNG/... -> 16-byte header + 200x200 RGB565 (little endian), centre-cropped
    square. The pod's panel only stores 5/6/5 bits per channel, so the conversion rounds
    properly (white -> 0xFFFF) and Floyd-Steinberg dithers instead of truncating; a light
    unsharp plus an optional colour boost bring back what the small screen flattens."""
    from PIL import Image, ImageEnhance, ImageFilter
    im = Image.open(io.BytesIO(cover_bytes))
    im.load()
    im = _to_srgb(im)
    w, h = im.size
    s = min(w, h)
    im = im.crop(((w - s) // 2, (h - s) // 2, (w - s) // 2 + s, (h - s) // 2 + s))
    if s > COVER_PX * 2:
        im = im.resize((COVER_PX * 2, COVER_PX * 2), Image.LANCZOS)
    im = im.resize((COVER_PX, COVER_PX), Image.LANCZOS)
    if sharpen:
        im = im.filter(ImageFilter.UnsharpMask(radius=0.8, percent=70, threshold=2))
    if saturation and abs(saturation - 1.0) > 0.01:
        im = ImageEnhance.Color(im).enhance(saturation)
    raw = im.tobytes()
    # Real-world JPEGs keep 246-254 "mush" around big white (or black) fields; on the
    # pod that mush is what triggers visible dither grain / tinted whites. Snap the
    # neutral extremes before quantizing - the panel can only show 5/6/5 bits anyway.
    raw = bytearray(raw)
    for i in range(0, len(raw), 3):
        r, g, b = raw[i], raw[i + 1], raw[i + 2]
        if r >= 246 and g >= 246 and b >= 246:
            raw[i] = raw[i + 1] = raw[i + 2] = 255
        elif r <= 5 and g <= 5 and b <= 5:
            raw[i] = raw[i + 1] = raw[i + 2] = 0
    raw = bytes(raw)
    if dither:
        words = _rgb565_dithered(raw, COVER_PX, COVER_PX)
    else:
        words = [((round(raw[i] * 31 / 255) << 11) | (round(raw[i + 1] * 63 / 255) << 5)
                  | round(raw[i + 2] * 31 / 255))
                 for i in range(0, len(raw), 3)]
    px = array.array("H", words)
    if sys.byteorder == "big":
        px.byteswap()
    header = b"MCOV" + struct.pack("<HHII", COVER_PX, COVER_PX, mp3_size & 0xFFFFFFFF, 0)
    return header + px.tobytes()


def blob_to_image(blob):
    """For tests / preview of what the iPod will show. The 5/6/5 channel values are
    expanded back with the exact per-level spacing (31 -> 255 is 8.225..., not <<3)."""
    from PIL import Image
    px = array.array("H")
    px.frombytes(blob[16:])
    out = bytearray()
    for v in px:
        out += bytes(((v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31))
    return Image.frombytes("RGB", (COVER_PX, COVER_PX), bytes(out))


def stripped_copy(path):
    """Temp copy of the mp3 without embedded pictures (tags are kept, ID3v2.3).
    Returns (path, is_temp). Falls back to the original on any problem."""
    try:
        from mutagen.id3 import ID3, ID3NoHeaderError
        try:
            tags = ID3(path)
        except ID3NoHeaderError:
            return path, False
        if not tags.getall("APIC"):
            return path, False
        fd, tmp = tempfile.mkstemp(suffix=".mp3")
        os.close(fd)
        shutil.copyfile(path, tmp)
        t = ID3(tmp)
        t.delall("APIC")
        t.save(tmp, v2_version=3)
        return tmp, True
    except Exception:
        return path, False
