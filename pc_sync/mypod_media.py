"""Reads MP3 tags, builds the iPod-friendly file name and the 200x200 RGB565 cover."""
import array
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


def make_cover_blob(cover_bytes, mp3_size):
    """Any JPEG/PNG/... -> 16-byte header + 200x200 RGB565 (little endian), centre-cropped square."""
    from PIL import Image
    import io
    im = Image.open(io.BytesIO(cover_bytes)).convert("RGB")
    w, h = im.size
    s = min(w, h)
    im = im.crop(((w - s) // 2, (h - s) // 2, (w - s) // 2 + s, (h - s) // 2 + s))
    im = im.resize((COVER_PX, COVER_PX), Image.LANCZOS)
    raw = im.tobytes()
    px = array.array("H", ((((raw[i] & 0xF8) << 8) | ((raw[i + 1] & 0xFC) << 3) | (raw[i + 2] >> 3))
                           for i in range(0, len(raw), 3)))
    if sys.byteorder == "big":
        px.byteswap()
    header = b"MCOV" + struct.pack("<HHII", COVER_PX, COVER_PX, mp3_size & 0xFFFFFFFF, 0)
    return header + px.tobytes()


def blob_to_image(blob):
    """For tests / preview of what the iPod will show."""
    from PIL import Image
    px = array.array("H")
    px.frombytes(blob[16:])
    out = bytearray()
    for v in px:
        out += bytes(((v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3))
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
