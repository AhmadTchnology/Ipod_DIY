"""Batch transfer logic (no GUI), so it can be tested against the device engine."""
import os

import mypod_media as M
import mypod_proto as P


def human(n):
    for u in ("B", "KB", "MB", "GB"):
        if n < 1024 or u == "GB":
            return f"{n:.0f} {u}" if u == "B" else f"{n:.1f} {u}"
        n /= 1024


def run_batch(dev, items, strip, skip_existing, cancel, on_row, on_progress, say):
    """items: list of (index, track) with track = {path, name, info{cover,...}}.
    Returns (sent, already_there, failed)."""
    existing = {s["path"]: s for s in dev.list_songs()}
    used = set()
    total_bytes = sum(os.path.getsize(t["path"]) for _, t in items)
    sent_bytes = 0
    sent = there = failed = 0
    for n, (idx, t) in enumerate(items, 1):
        if cancel():
            break
        src, is_tmp = M.stripped_copy(t["path"]) if strip else (t["path"], False)
        try:
            size = os.path.getsize(src)
            dpath = f"/music/{t['name']}.mp3"
            k = 2
            while dpath in used:                        # two different files with the same tags
                dpath = f"/music/{t['name']} ({k}).mp3"
                k += 1
            used.add(dpath)
            old = existing.get(dpath)
            same = bool(old) and old["size"] == size
            label = f"[{n}/{len(items)}] {t['name']}"
            blob = None
            if t["info"].get("cover"):
                try:
                    blob = M.make_cover_blob(t["info"]["cover"], size)
                except Exception as e:
                    say(f"  cover of '{t['name']}' not readable ({e}) - sending without it")
            if skip_existing and same:
                there += 1
                status = "already there"
            else:
                if old and not same:
                    say(f"  replacing '{t['name']}' (different version on the iPod)")
                on_row(idx, "sending...")
                base = sent_bytes

                def prog(d, tot, base=base, label=label):
                    on_progress(d, tot, base + d, total_bytes, f"{label}   {human(d)} / {human(tot)}")
                dev.put(dpath, src, progress=prog, cancel=cancel)
                sent += 1
                status = "done"
            if blob and not (skip_existing and same and old.get("cover")):
                dev.put(M.cover_dev_path(dpath), blob, cancel=cancel)
            on_row(idx, status)
        except P.Cancelled:
            on_row(idx, "cancelled")
            raise
        except Exception as e:
            failed += 1
            on_row(idx, "FAILED")
            say(f"  {t['name']}: {e}")
        finally:
            if is_tmp:
                try:
                    os.remove(src)
                except OSError:
                    pass
        sent_bytes += os.path.getsize(t["path"])
    return sent, there, failed
