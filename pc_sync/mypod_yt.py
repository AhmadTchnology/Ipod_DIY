"""YouTube support for MyPod Sync: search, download, convert to MP3, tag.

No GUI in this module - mypod_sync.py calls it from worker threads.
Needs: yt-dlp (pip), and an ffmpeg binary for the MP3 conversion
(PATH, the saved "ffmpeg" setting, or the bundled imageio-ffmpeg wheel).
"""
import json
import os
import re
import shutil
import sys
import urllib.request

import mypod_media as M
import mypod_proto as P

UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"

# Folder for the user's files: next to the script, or next to the exe when
# frozen with PyInstaller (a one-file exe extracts to a temp dir that is
# deleted on exit, so __file__ must never be used for saving anything there).
if getattr(sys, "frozen", False):
    APP_DIR = os.path.dirname(os.path.abspath(sys.executable))
else:
    APP_DIR = os.path.dirname(os.path.abspath(__file__))
CONFIG_FILE = os.path.join(APP_DIR, "mypod_cfg.json")

# bracket suffixes YouTube adds: "(Official Music Video)", "[Lyric Audio HD]", ...
_JUNK_BRACKET = re.compile(
    r"\s*[(\[【](?=[^)\]】]*\b(official|video|audio|lyrics?|lyric |mv\b|m/v|hd\b|hq\b|4k|8k|"
    r"visualizer|full album|explicit|clean|re-?master|dvd|bonus|subscrib|copyright|preview)\b"
    r")[^)\]】]*[)\]】]", re.I)
_JUNK_TAIL = re.compile(r"\s*[-–|]\s*(official\s+)?(music\s+)?(video|audio|lyrics?)\s*$", re.I)


def clean_title(s):
    """'Song Name (Official Music Video)' -> 'Song Name'.  Conservative: only strips
    bracketed parts that contain one of the known filler keywords."""
    s = _JUNK_BRACKET.sub("", s or "")
    s = _JUNK_TAIL.sub("", s)
    s = re.sub(r"\s{2,}", " ", s).strip(" \t-–—")
    return s.strip()


def dedupe_artist(title, artist):
    """'Ed Sheeran - Shape of You' + artist 'Ed Sheeran'  ->  'Shape of You'."""
    t, a = (title or "").strip(), (artist or "").strip()
    if a:
        m = re.match(r"^" + re.escape(a) + r"\s+[-–—]\s+(.+)$", t, re.I)
        if m:
            return m.group(1).strip()
    return t


def split_artist_title(raw, channel=""):
    """Common YouTube convention 'Artist - Song'  ->  (artist, song),
    falling back to (channel, raw) when there is no such pattern."""
    t = (raw or "").strip()
    m = re.match(r"^(.{1,60}?)\s+[-–—]\s+(.+)$", t)
    if m:
        return m.group(1).strip(), m.group(2).strip()
    return (channel or "").strip(), t


def load_cfg():
    try:
        with open(CONFIG_FILE, encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}


def save_cfg(cfg):
    try:
        with open(CONFIG_FILE, "w", encoding="utf-8") as f:
            json.dump(cfg, f)
    except Exception:
        pass


def resolve_ffmpeg(saved=None):
    """-> path to a usable ffmpeg binary, or None. Order: saved setting, PATH,
    the binary bundled in a frozen (PyInstaller) build, then the binary that
    the ``imageio-ffmpeg`` pip package bundles."""
    if saved and os.path.isfile(saved):
        return saved
    found = shutil.which("ffmpeg")
    if found:
        return found
    if getattr(sys, "frozen", False):
        bdir = os.path.join(getattr(sys, "_MEIPASS", ""), "imageio_ffmpeg", "binaries")
        if os.path.isdir(bdir):
            for fn in sorted(os.listdir(bdir)):
                p = os.path.join(bdir, fn)
                if fn.lower().startswith("ffmpeg") and os.path.isfile(p):
                    return p
    try:
        import imageio_ffmpeg
        return imageio_ffmpeg.get_ffmpeg_exe()
    except Exception:
        return None


def cache_dir(base=None):
    d = base or os.path.join(APP_DIR, "downloads")
    os.makedirs(d, exist_ok=True)
    return d


def cached_mp3(vid, base=None):
    """Path of an already-downloaded song of decent size, else None."""
    p = os.path.join(cache_dir(base), vid + ".mp3")
    return p if os.path.isfile(p) and os.path.getsize(p) > 100 * 1024 else None


def search(query, limit=20):
    """-> [{id, url, title, song_title, channel, duration}] (flat, fast)."""
    import yt_dlp
    opts = {"quiet": True, "no_warnings": True, "skip_download": True,
            "extract_flat": "in_playlist", "noprogress": True, "socket_timeout": 15}
    with yt_dlp.YoutubeDL(opts) as ydl:
        info = ydl.extract_info(f"ytsearch{limit}:{query}", download=False)
    out, seen = [], set()
    for e in (info.get("entries") or []):
        if not e:
            continue
        vid = e.get("id") or ""
        if not vid or vid in seen:
            continue
        seen.add(vid)
        raw = e.get("title") or "(untitled)"
        out.append({
            "id": vid,
            "url": f"https://www.youtube.com/watch?v={vid}",
            "title": raw,
            "song_title": clean_title(raw),
            "channel": e.get("uploader") or e.get("channel") or "",
            "duration": int(e.get("duration") or 0),
        })
    return out


def resolve_flat(url):
    """Quick metadata for one URL (title/channel/duration), None when unsuccessful.
    Enough to give a queued song a sensible name before it is downloaded."""
    import yt_dlp
    opts = {"quiet": True, "no_warnings": True, "skip_download": True,
            "extract_flat": True, "noprogress": True, "socket_timeout": 10}
    try:
        with yt_dlp.YoutubeDL(opts) as ydl:
            info = ydl.extract_info(url, download=False)
    except Exception:
        return None
    if not info:
        return None
    raw = info.get("title") or ""
    return {"title": raw, "song_title": clean_title(raw),
            "channel": info.get("uploader") or info.get("channel") or "",
            "duration": int(info.get("duration") or 0)}


def _youtube_id(url):
    """>>> video id of a youtube.com/watch?v=.. or youtu.be/.. link, else None."""
    try:
        from urllib.parse import urlparse, parse_qs
        u = urlparse(url)
        if "youtu.be" in u.netloc.lower():
            seg = u.path.lstrip("/")
            if 6 <= len(seg) <= 20 and re.fullmatch(r"[\w-]+", seg):
                return seg
        v = parse_qs(u.query).get("v", [])
        if v and re.fullmatch(r"[\w-]{6,}", v[0]):
            return v[0]
    except Exception:
        pass
    return None


def parse_m3u(path):
    """Read an .m3u/.m3u8 playlist.  -> entries:
       {"kind": "file", "path": ...}                - a local MP3
       {"kind": "url",  "meta": {...}}              - YouTube (or any yt-dlp site) song
    Relative paths are resolved against the playlist's folder; leftover lines
    are reported as "missing".  UTF-8 (with/without BOM) and cp1252 are tried."""
    data = None
    for enc in ("utf-8-sig", "utf-8", "cp1252"):
        try:
            with open(path, encoding=enc) as f:
                data = f.read()
            break
        except UnicodeDecodeError:
            continue
    if data is None:
        raise RuntimeError("could not decode the playlist (unknown encoding)")

    entries, missing = [], []
    base = os.path.dirname(os.path.abspath(path))
    pending = {}                        # EXTINF info waiting for its URL/line
    for line in data.splitlines():
        line = line.strip()
        if not line:
            continue
        if line.startswith("#EXTINF:"):
            body = line[len("#EXTINF:"):]
            dur_s, _, title = body.partition(",")
            pending = {"duration": int(dur_s) if dur_s.strip().lstrip("-").isdigit() else 0,
                       "title": title.strip()}
        elif line.startswith("#"):
            continue
        elif "://" in line:
            url = line
            meta = {"id": _youtube_id(url) or url, "url": url, "title": "" or pending.get("title", ""),
                    "song_title": clean_title(pending.get("title", "")), "channel": "",
                    "duration": pending.get("duration", 0)}
            entries.append({"kind": "url", "meta": meta, "ref": url})
            pending = {}
        elif "\\" in line or "/" in line or line.lower().endswith(".mp3"):
            p = line if os.path.isabs(line) else os.path.join(base, line)
            p = os.path.normpath(p)
            if os.path.isfile(p):
                entries.append({"kind": "file", "path": p})
            else:
                missing.append(line)
            pending = {}
        else:
            missing.append(line)
            pending = {}
    return {"entries": entries, "missing": missing}


def fetch_thumb(vid, thumb_dir=None):
    """Square-ish list thumbnail (cached), None if it could not be fetched."""
    d = thumb_dir or os.path.join(cache_dir(), "thumbs")
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, vid + ".jpg")
    if os.path.isfile(p):
        return p
    for url in (f"https://i.ytimg.com/vi/{vid}/mqdefault.jpg",
                f"https://i.ytimg.com/vi/{vid}/hqdefault.jpg"):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": UA})
            with urllib.request.urlopen(req, timeout=10) as r:
                data = r.read()
            if len(data) > 500:
                with open(p, "wb") as f:
                    f.write(data)
                return p
        except Exception:
            continue
    return None


def _cover_bytes(info, say):
    """The best thumbnail of the video as JPEG bytes (for the /art cover)."""
    url = info.get("thumbnail") or next(
        (t.get("url") for t in reversed(info.get("thumbnails") or []) if t.get("url")), None)
    if not url:
        return None
    try:
        req = urllib.request.Request(url, headers={"User-Agent": UA})
        with urllib.request.urlopen(req, timeout=15) as r:
            data = r.read()
    except Exception as e:
        if say:
            say(f"  cover download failed ({e}) - sending without it")
        return None
    if data[:2] != b"\xff\xd8":                       # webp/other -> JPEG via Pillow
        try:
            import io
            from PIL import Image
            im = Image.open(io.BytesIO(data)).convert("RGB")
            buf = io.BytesIO()
            im.save(buf, "JPEG", quality=85)
            data = buf.getvalue()
        except Exception:
            return None
    return data if len(data) > 500 else None


def _tag(mp3_path, info, say=None):
    """Write YouTube metadata + cover into the MP3 so read_info() sees them."""
    from mutagen.id3 import APIC, ID3, ID3NoHeaderError, PictureType, TALB, TIT2, TPE1
    title = clean_title(info.get("title") or "")
    artist = (info.get("artist") or info.get("creator") or info.get("uploader")
              or info.get("channel") or "")
    title = dedupe_artist(title, artist)          # 'Ed Sheeran - Song' + artist 'Ed Sheeran'
    album = info.get("album") or ""
    try:
        try:
            tags = ID3(mp3_path)
        except ID3NoHeaderError:
            tags = ID3()
        for k in ("TIT2", "TPE1", "TALB", "APIC"):
            tags.delall(k)
        if title:
            tags.add(TIT2(encoding=3, text=title))
        if artist:
            tags.add(TPE1(encoding=3, text=artist))
        if album:
            tags.add(TALB(encoding=3, text=album))
        cover = _cover_bytes(info, say)
        if cover:
            tags.add(APIC(encoding=3, mime="image/jpeg", type=PictureType.COVER_FRONT,
                          desc="Cover", data=cover))
        tags.save(mp3_path, v2_version=3)
    except Exception as e:
        if say:
            say(f"  tagging failed ({e}) - the song is saved without tags")


def download(url, out_dir=None, bitrate=192, ffmpeg=None, cancel=None, progress=None, say=None):
    """Download one video as <id>.mp3 at the requested quality, tag it, and
    return (path, info).  Raises P.Cancelled when cancel() turns True."""
    import yt_dlp
    dd = cache_dir(out_dir)
    ff = resolve_ffmpeg(ffmpeg)
    if not ff:
        raise RuntimeError(
            "ffmpeg was not found (it converts the audio to MP3). Install ffmpeg, run "
            "pip install imageio-ffmpeg, or pick ffmpeg.exe in the YouTube tab.")

    def hook(d):
        if cancel and cancel():
            raise P.Cancelled()
        if progress and d.get("status") == "downloading":
            progress(d.get("downloaded_bytes", 0),
                     d.get("total_bytes") or d.get("total_bytes_estimate") or 0)

    opts = {"quiet": True, "no_warnings": True, "noprogress": True,
            # NB: plain string - the {"default": ...} dict form skips
            # postprocessors in some yt-dlp versions (2026.10.x)
            "outtmpl": os.path.join(dd, "%(id)s.%(ext)s"),
            "format": "bestaudio/best", "retries": 3, "socket_timeout": 20,
            "ffmpeg_location": ff, "progress_hooks": [hook],
            "postprocessors": [{"key": "FFmpegExtractAudio",
                                "preferredcodec": "mp3",
                                "preferredquality": str(bitrate)}]}
    with yt_dlp.YoutubeDL(opts) as ydl:
        info = ydl.extract_info(url, download=True)
        info = ydl.sanitize_info(info)
    if cancel and cancel():
        raise P.Cancelled()
    final = os.path.join(dd, info["id"] + ".mp3")
    if not os.path.isfile(final):
        raise RuntimeError("the MP3 was not produced (check ffmpeg)")
    _tag(final, info, say)
    return final, info
