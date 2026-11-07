#!/usr/bin/env python3
"""MyPod Sync - copy MP3s (with tags and album covers) to the DIY iPod over USB.

On the iPod:  Settings > "Copy songs from PC (USB)..." > Restart.   Then run this program,
pick the COM port, press Connect, add songs, press Send.
"""
import os
import queue
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

import mypod_proto as P
import mypod_media as M
import mypod_yt as YT
from mypod_batch import run_batch, human

APP = "MyPod Sync"
SPEEDS = ["921600", "1500000", "2000000", "460800", "115200"]


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title(APP)
        sw, sh = self.winfo_screenwidth(), self.winfo_screenheight()
        w, h = min(1020, sw - 40), min(700, sh - 90)
        self.geometry(f"{w}x{h}+{max(0, (sw - w) // 2)}+{max(0, (sh - h) // 3)}")
        self.minsize(720, 480)
        self.q = queue.Queue()
        self.dev = None
        self.busy = False
        self.cancel_flag = False
        self.tracks = []              # dicts: path, info, name, status
        self.device_songs = {}        # dev path -> {size, cover}
        self.preview_img = None
        self.cfg = YT.load_cfg()
        self.yt_results = []
        self.yt_thumbs = {}           # iid -> PhotoImage (keep referenced)
        self.searching = False
        self._build()
        self.refresh_ports()
        self.after(100, self._pump)
        self.protocol("WM_DELETE_WINDOW", self._close)

    # ------------------------------------------------------------------ UI
    def _build(self):
        top = ttk.Frame(self, padding=8)
        top.pack(fill="x")
        ttk.Label(top, text="USB port:").pack(side="left")
        self.port_var = tk.StringVar()
        self.port_cb = ttk.Combobox(top, textvariable=self.port_var, width=34, state="readonly")
        self.port_cb.pack(side="left", padx=4)
        ttk.Button(top, text="Refresh", command=self.refresh_ports).pack(side="left")
        ttk.Label(top, text="  Speed:").pack(side="left")
        self.speed_var = tk.StringVar(value=SPEEDS[0])
        ttk.Combobox(top, textvariable=self.speed_var, values=SPEEDS, width=9, state="readonly").pack(side="left", padx=4)
        self.conn_btn = ttk.Button(top, text="Connect", command=self.toggle_connect)
        self.conn_btn.pack(side="left", padx=8)
        self.status_var = tk.StringVar(value="Not connected")
        ttk.Label(top, textvariable=self.status_var, foreground="#555").pack(side="left", padx=8)

        # --- bottom (packed FIRST so the buttons always stay on screen, even on
        #     small displays: the song list gets whatever is left over)
        log = tk.Text(self, height=5, state="disabled", background="#f6f6f6")
        log.pack(fill="x", side="bottom", padx=8, pady=(4, 8))
        self.log = log
        pf = ttk.Frame(self, padding=(8, 0))
        pf.pack(fill="x", side="bottom")
        self.file_bar = ttk.Progressbar(pf, maximum=100)
        self.file_bar.pack(fill="x", pady=2)
        self.all_bar = ttk.Progressbar(pf, maximum=100)
        self.all_bar.pack(fill="x", pady=2)
        self.prog_var = tk.StringVar()
        ttk.Label(pf, textvariable=self.prog_var, foreground="#555").pack(anchor="w")
        bot = ttk.Frame(self, padding=8)
        bot.pack(fill="x", side="bottom")
        self.send_btn = ttk.Button(bot, text="Send to iPod", command=self.send_all)
        self.send_btn.pack(side="left")
        self.dl_btn = ttk.Button(bot, text="Download now (no iPod needed)", command=self.download_now)
        self.dl_btn.pack(side="left", padx=4)
        self.cancel_btn = ttk.Button(bot, text="Cancel", command=self._cancel, state="disabled")
        self.cancel_btn.pack(side="left", padx=4)
        self.exit_btn = ttk.Button(bot, text="Finish & start the player", command=self.exit_player)
        self.exit_btn.pack(side="right")

        nb = ttk.Notebook(self)
        nb.pack(fill="both", expand=True, padx=8, pady=4)
        self.nb = nb
        ttk.Style(self).configure("Yt.Treeview", rowheight=52)

        # --- tab 1: songs to send
        t1 = ttk.Frame(nb, padding=6)
        nb.add(t1, text="Songs to send")
        bar = ttk.Frame(t1)
        bar.pack(fill="x")
        ttk.Button(bar, text="Add files...", command=self.add_files).pack(side="left")
        ttk.Button(bar, text="Add folder...", command=self.add_folder).pack(side="left", padx=4)
        ttk.Button(bar, text="Import .m3u playlist...", command=self.import_m3u).pack(side="left")
        ttk.Button(bar, text="Remove selected", command=self.remove_selected).pack(side="left", padx=4)
        ttk.Button(bar, text="Clear list", command=self.clear_list).pack(side="left")
        body = ttk.Frame(t1)
        body.pack(fill="both", expand=True, pady=6)
        cols = ("title", "artist", "album", "time", "size", "cover", "status")
        self.tree = ttk.Treeview(body, columns=cols, show="headings", selectmode="extended")
        for c, w, t in (("title", 220, "Title"), ("artist", 150, "Artist"), ("album", 150, "Album"),
                        ("time", 50, "Time"), ("size", 70, "Size"), ("cover", 50, "Cover"), ("status", 140, "Status")):
            self.tree.heading(c, text=t)
            self.tree.column(c, width=w, anchor="w")
        sb = ttk.Scrollbar(body, command=self.tree.yview)
        self.tree.configure(yscrollcommand=sb.set)
        self.tree.pack(side="left", fill="both", expand=True)
        sb.pack(side="left", fill="y")
        self.tree.bind("<<TreeviewSelect>>", self._on_select)
        side = ttk.Frame(body, padding=(10, 0))
        side.pack(side="left", fill="y")
        self.cover_lbl = ttk.Label(side, text="(cover preview)", anchor="center", width=28)
        self.cover_lbl.pack()
        self.detail_var = tk.StringVar()
        ttk.Label(side, textvariable=self.detail_var, wraplength=210, justify="left").pack(pady=8, anchor="w")

        opt = ttk.LabelFrame(t1, text="Options", padding=6)
        opt.pack(fill="x")
        self.strip_var = tk.BooleanVar(value=True)
        self.skip_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(opt, variable=self.strip_var,
                        text="Remove the big embedded picture from the MP3 copy (faster, saves card space; "
                             "the cover is still sent and shown on the iPod)").pack(anchor="w")
        ttk.Checkbutton(opt, variable=self.skip_var,
                        text="Skip songs that are already on the iPod (same name and size)").pack(anchor="w")

        # --- tab 2: YouTube search -> add to the send list
        ty = ttk.Frame(nb, padding=6)
        nb.add(ty, text="YouTube")
        sb2 = ttk.Frame(ty)
        sb2.pack(fill="x")
        ttk.Label(sb2, text="Search:").pack(side="left")
        self.yt_var = tk.StringVar()
        self.yt_entry = ttk.Entry(sb2, textvariable=self.yt_var)
        self.yt_entry.pack(side="left", fill="x", expand=True, padx=4)
        self.yt_entry.bind("<Return>", lambda _e: self.yt_search())
        self.yt_go = ttk.Button(sb2, text="Search", command=self.yt_search)
        self.yt_go.pack(side="left")
        oy = ttk.Frame(ty)
        oy.pack(fill="x", pady=(6, 0))
        ttk.Label(oy, text="Audio quality:").pack(side="left")
        self.yt_bitrate_var = tk.StringVar(value="192")
        ttk.Combobox(oy, textvariable=self.yt_bitrate_var, values=("128", "192", "256", "320"),
                     width=5, state="readonly").pack(side="left", padx=(4, 16))
        self.yt_ff_var = tk.StringVar(value=self._ffmpeg_status_text())
        ttk.Label(oy, textvariable=self.yt_ff_var, foreground="#555").pack(side="left")
        ttk.Button(oy, text="Pick ffmpeg.exe...", command=self.pick_ffmpeg).pack(side="left", padx=4)
        body_y = ttk.Frame(ty)
        body_y.pack(fill="both", expand=True, pady=6)
        self.yt_tree = ttk.Treeview(body_y, columns=("title", "channel", "time"),
                                    style="Yt.Treeview", show="tree headings",
                                    selectmode="extended")
        self.yt_tree.heading("#0", text="")
        self.yt_tree.column("#0", width=58, minwidth=58, stretch=False, anchor="center")
        for c, w, t in (("title", 420, "Title"), ("channel", 190, "Channel"), ("time", 60, "Time")):
            self.yt_tree.heading(c, text=t)
            self.yt_tree.column(c, width=w, anchor="w")
        ys = ttk.Scrollbar(body_y, command=self.yt_tree.yview)
        self.yt_tree.configure(yscrollcommand=ys.set)
        self.yt_tree.pack(side="left", fill="both", expand=True)
        ys.pack(side="left", fill="y")
        self.yt_tree.bind("<Double-1>", lambda _e: self._yt_add(self._yt_rows(self.yt_tree.selection()), True))
        btns_y = ttk.Frame(ty)
        btns_y.pack(fill="x")
        ttk.Button(btns_y, text="Add selected to send list",
                   command=lambda: self._yt_add(self._yt_rows(self.yt_tree.selection()), False)).pack(side="left")
        ttk.Button(btns_y, text="Add all shown", command=lambda: self._yt_add(self.yt_results, False)).pack(side="left", padx=4)
        ttk.Label(btns_y, text="Then press 'Download now' (fetch without the iPod) or 'Send to iPod' "
                               "(download + transfer in one go) at the bottom of the window.",
                  foreground="#888").pack(side="left", padx=6)

        # --- tab 3: on the iPod
        t2 = ttk.Frame(nb, padding=6)
        nb.add(t2, text="On the iPod")
        bar2 = ttk.Frame(t2)
        bar2.pack(fill="x")
        ttk.Button(bar2, text="Refresh list", command=self.refresh_device).pack(side="left")
        ttk.Button(bar2, text="Delete selected", command=self.delete_selected).pack(side="left", padx=4)
        self.dev_tree = ttk.Treeview(t2, columns=("path", "size", "cover"), show="headings", selectmode="extended")
        for c, w, t in (("path", 560, "File on the iPod"), ("size", 90, "Size"), ("cover", 70, "Cover")):
            self.dev_tree.heading(c, text=t)
            self.dev_tree.column(c, width=w, anchor="w")
        self.dev_tree.pack(fill="both", expand=True, pady=6)

    # ------------------------------------------------------------------ helpers
    def say(self, text):
        self.q.put(("log", text))

    def _pump(self):
        try:
            while True:
                kind, *a = self.q.get_nowait()
                getattr(self, "_ev_" + kind)(*a)
        except queue.Empty:
            pass
        self.after(100, self._pump)

    def _ev_log(self, text):
        self.log.configure(state="normal")
        self.log.insert("end", text + "\n")
        self.log.see("end")
        self.log.configure(state="disabled")

    def _ev_status(self, text):
        self.status_var.set(text)

    def _ev_error(self, text):
        self._ev_log("ERROR: " + text)
        messagebox.showerror(APP, text)

    def _ev_connected(self, ok):
        self.conn_btn.configure(text="Disconnect" if ok else "Connect")
        if not ok:
            self.status_var.set("Not connected")

    def _ev_busy(self, b):
        self.busy = b
        self.cancel_btn.configure(state="normal" if b else "disabled")

    def _ev_progress(self, fdone, ftotal, adone, atotal, text):
        self.file_bar["value"] = 100 * fdone / ftotal if ftotal else 0
        self.all_bar["value"] = 100 * adone / atotal if atotal else 0
        self.prog_var.set(text)

    def _ev_row(self, idx, status):
        t = self.tracks[idx]
        t["status"] = status
        if "iid" in t:
            self.tree.set(t["iid"], "status", status)

    def _ev_ytresults(self, results, err):
        self.searching = False
        self.yt_go.configure(state="normal", text="Search")
        if err:
            self._ev_log("Search failed: " + err)
            hint = ""
            try:
                import yt_dlp
            except Exception:
                hint = "\n\n(the yt-dlp module is missing:  pip install yt-dlp)"
            messagebox.showerror(APP, "YouTube search failed:\n" + err + hint)
            return
        self.yt_results = results
        for r in results:
            r["iid"] = self.yt_tree.insert("", "end", values=(
                r["title"], r["channel"],
                f"{r['duration'] // 60}:{r['duration'] % 60:02d}" if r["duration"] else "-"))
        self.say(f"{len(results)} result(s).")
        threading.Thread(target=self._load_thumbs, daemon=True).start()

    def _load_thumbs(self):
        import time
        for r in list(self.yt_results):
            p = YT.fetch_thumb(r["id"])
            if p:
                self.q.put(("thumb", r["iid"], p))
            time.sleep(0.05)

    def _ev_thumb(self, iid, path):
        if not self.yt_tree.exists(iid):
            return
        try:
            from PIL import Image, ImageTk
            im = Image.open(path).convert("RGB")
            w, h = im.size
            s = min(w, h)
            im = im.crop(((w - s) // 2, (h - s) // 2, (w + s) // 2, (h + s) // 2)).resize((48, 48))
            ph = ImageTk.PhotoImage(im)
            self.yt_thumbs[iid] = ph
            self.yt_tree.item(iid, image=ph)
        except Exception:
            pass

    def _ev_ytprog(self, pct, text):
        self.file_bar["value"] = pct
        self.prog_var.set(text)

    def _ev_ytdone(self, idx, path):
        """Row refresh only - the worker already set t['path']/t['info']/t['name']."""
        t = self.tracks[idx]
        if not t.get("iid"):
            return
        info = t["info"]
        secs = info["seconds"]
        self.tree.set(t["iid"], "title", info["title"] or t["stem"])
        self.tree.set(t["iid"], "artist", info["artist"])
        self.tree.set(t["iid"], "time", f"{secs // 60}:{secs % 60:02d}" if secs else "")
        self.tree.set(t["iid"], "size", human(os.path.getsize(path)))
        self.tree.set(t["iid"], "cover", "yes" if info["cover"] else "-")

    def _ffmpeg_status_text(self):
        ff = YT.resolve_ffmpeg(self.cfg.get("ffmpeg"))
        self.ffmpeg_path = ff
        if ff:
            name = os.path.basename(ff)
            if "imageio" in ff.lower():
                name += " (bundled)"
            return "ffmpeg: " + name
        return "ffmpeg NOT found - needed for MP3 conversion"

    def pick_ffmpeg(self):
        f = filedialog.askopenfilename(
            title="Pick the ffmpeg binary (ffmpeg.exe on Windows)",
            filetypes=[("ffmpeg", "ffmpeg*"), ("All files", "*.*")])
        if not f:
            return
        self.cfg["ffmpeg"] = f
        YT.save_cfg(self.cfg)
        self.yt_ff_var.set(self._ffmpeg_status_text())

    def yt_search(self):
        if self.searching:
            return
        q = self.yt_var.get().strip()
        if not q:
            return
        self.searching = True
        self.yt_go.configure(state="disabled", text="Searching...")
        self.yt_tree.delete(*self.yt_tree.get_children())
        self.yt_thumbs.clear()
        self.say(f"Searching YouTube for '{q}'...")

        def job():
            try:
                self.q.put(("ytresults", YT.search(q, 20), None))
            except Exception as e:
                self.q.put(("ytresults", [], str(e)))
        threading.Thread(target=job, daemon=True).start()

    def _yt_rows(self, iids):
        sel = set(iids)
        return [r for r in self.yt_results if r.get("iid") in sel]

    def _yt_track_add(self, r):
        """Queue one YouTube-source item (deduped by id). Returns True if added."""
        have = {t["yt"]["id"] for t in self.tracks if t.get("yt") and t["yt"].get("id")}
        if r.get("id") in have:
            return False
        cached = YT.cached_mp3(r["id"]) if r.get("id") else None
        if cached:
            info = M.read_info(cached)
        else:                                       # best guess until it is downloaded
            a_guess, t_guess = YT.split_artist_title(r["song_title"] or r["title"], r["channel"])
            info = {"title": t_guess, "artist": a_guess,
                    "album": "", "seconds": r["duration"], "bitrate": 0, "cover": None}
        t = {"yt": r, "path": cached,
             "status": "ready (cached)" if cached else "needs download",
             "stem": r["song_title"] or r["title"] or "(untitled)", "info": info}
        t["name"] = M.device_name(info["artist"], info["title"], t["stem"])
        secs = info["seconds"]
        t["iid"] = self.tree.insert("", "end", values=(
            info["title"] or t["stem"], info["artist"], info["album"],
            f"{secs // 60}:{secs % 60:02d}" if secs else "",
            human(os.path.getsize(cached)) if cached else "-",
            "yes" if info["cover"] else "-", t["status"]))
        self.tracks.append(t)
        return True

    def _yt_add(self, items, jump):
        if not items:
            return
        added = sum(1 for r in items if self._yt_track_add(r))
        self.say(f"Added {added} YouTube song(s) to the send list.")
        if added:
            self.nb.select(0)

    def _ev_ytenqueue(self, r):
        if self._yt_track_add(r):
            self.say(f"  queued: {r['song_title'] or r['title']}")

    def import_m3u(self):
        f = filedialog.askopenfilename(
            title="Choose an .m3u playlist (exported from YouTube or a media player)",
            filetypes=[("M3U playlist", "*.m3u *.m3u8"), ("All files", "*.*")])
        if not f:
            return
        self.cancel_flag = False

        def job():
            try:
                parsed = YT.parse_m3u(f)
            except Exception as e:
                self.q.put(("error", f"Could not read the playlist: {e}"))
                return
            entries, missing = parsed["entries"], parsed["missing"]
            self.say(f"Importing '{os.path.basename(f)}': {len(entries)} track(s)"
                     + (f" ({len(missing)} unresolvable line(s) skipped)" if missing else ""))
            for m in missing[:5]:
                self.say(f"  skipped (not a URL / no such file): {m[:70]}")
            n_file = n_url = 0
            for e in entries:
                if self.cancel_flag:
                    raise P.Cancelled()
                if e["kind"] == "file":
                    self.q.put(("addtrack", e["path"], M.read_info(e["path"])))
                    n_file += 1
                else:
                    r = e["meta"]
                    if not (r["song_title"] or r["title"]):      # plain URL without EXTINF info
                        flat = YT.resolve_flat(r["url"])
                        if flat:
                            r["title"] = flat["title"]
                            r["song_title"] = flat["song_title"]
                            r["channel"] = flat["channel"]
                            r["duration"] = flat["duration"]
                        else:
                            r["title"] = r["song_title"] = r.get("id", r["url"])[:60]
                            self.say(f"  could not look up the title of {r['url'][:70]}")
                    self.q.put(("ytenqueue", r))
                    n_url += 1
            self.q.put(("log", f"Playlist import finished: {n_file} local file(s), "
                               f"{n_url} song(s) to download."))
        self._run(job)

    def _ev_devlist(self, songs):
        self.device_songs = {s["path"]: s for s in songs}
        self.dev_tree.delete(*self.dev_tree.get_children())
        for s in sorted(songs, key=lambda s: s["path"].lower()):
            self.dev_tree.insert("", "end", values=(s["path"], human(s["size"]), "yes" if s["cover"] else "-"))
        self._ev_log(f"{len(songs)} song(s) on the iPod")

    def _need_dev(self):
        if not self.dev:
            messagebox.showinfo(APP, "Connect to the iPod first (Settings > Copy songs from PC on the iPod).")
            return False
        if self.busy:
            messagebox.showinfo(APP, "Please wait for the current job to finish.")
            return False
        return True

    def _run(self, fn):
        self.q.put(("busy", True))

        def wrap():
            try:
                fn()
            except P.Cancelled:
                self.say("Cancelled.")
            except Exception as e:
                self.q.put(("error", str(e)))
            finally:
                self.q.put(("busy", False))
        threading.Thread(target=wrap, daemon=True).start()

    # ------------------------------------------------------------------ connection
    def refresh_ports(self):
        try:
            from serial.tools import list_ports
            ports = [(p.device, f"{p.device} - {p.description}") for p in list_ports.comports()]
        except Exception:
            ports = []
        self._ports = dict((label, dev) for dev, label in ports)
        self.port_cb["values"] = list(self._ports)
        if ports and not self.port_var.get():
            self.port_var.set(ports[0][1])

    def toggle_connect(self):
        if self.dev:
            self._disconnect()
            return
        if self.busy:
            return
        label = self.port_var.get()
        if not label:
            messagebox.showinfo(APP, "Pick the iPod's USB port first.")
            return
        port = self._ports.get(label, label)
        speed = int(self.speed_var.get())
        self.q.put(("status", "Connecting..."))

        def job():
            tr = None
            try:
                tr = P.SerialTransport(port, 115200)
                dev = P.Device(tr, log=self.say)
                info = dev.hello()
                if not info["sd"]:
                    self.say("Warning: the iPod reports NO SD card.")
                if speed != 115200:
                    if dev.set_baud(speed):
                        self.say(f"Fast link at {speed} baud")
                self.dev = dev
                self.q.put(("connected", True))
                self.q.put(("status", f"Connected to {info['name']}  (card {info['card_mb']} MB)"))
                songs = dev.list_songs()
                self.q.put(("devlist", songs))
                try:
                    total, free = dev.free_space()
                    self.q.put(("status", f"Connected - card {human(total)}, {human(free)} free"))
                except Exception:
                    pass
            except Exception as e:
                if tr:
                    tr.close()
                self.dev = None
                self.q.put(("connected", False))
                raise e
        self._run(job)

    def _disconnect(self):
        if self.busy:
            return
        try:
            self.dev.t.close()
        except Exception:
            pass
        self.dev = None
        self._ev_connected(False)

    def exit_player(self):
        if not self._need_dev():
            return
        if not messagebox.askyesno(APP, "Restart the iPod into the music player now?"):
            return

        def job():
            self.dev.exit_to_player()
            self.say("iPod is restarting into the player. New songs appear after its library scan.")
            self.dev.t.close()
            self.dev = None
            self.q.put(("connected", False))
        self._run(job)

    # ------------------------------------------------------------------ song list
    def add_files(self):
        files = filedialog.askopenfilenames(title="Choose MP3 files", filetypes=[("MP3 audio", "*.mp3"), ("All files", "*.*")])
        self._add_paths(list(files))

    def add_folder(self):
        d = filedialog.askdirectory(title="Choose a folder with MP3 files")
        if not d:
            return
        found = []
        for root, _dirs, files in os.walk(d):
            found += [os.path.join(root, f) for f in sorted(files) if f.lower().endswith(".mp3")]
        self._add_paths(found)

    def _add_paths(self, paths):
        known = {t["path"] for t in self.tracks}
        paths = [p for p in paths if p not in known]
        non_mp3 = [p for p in paths if not p.lower().endswith(".mp3")]
        paths = [p for p in paths if p.lower().endswith(".mp3")]
        if non_mp3:
            self.say(f"Skipped {len(non_mp3)} non-MP3 file(s) - the iPod plays MP3 only (convert with ffmpeg/Audacity).")

        def job():
            for p in paths:
                info = M.read_info(p)
                self.q.put(("addtrack", p, info))
        threading.Thread(target=job, daemon=True).start()

    def _ev_addtrack(self, path, info):
        stem = os.path.splitext(os.path.basename(path))[0]
        t = {"path": path, "info": info, "stem": stem, "status": "ready",
             "name": M.device_name(info["artist"], info["title"], stem)}
        t["iid"] = self.tree.insert("", "end", values=(
            info["title"] or stem, info["artist"], info["album"],
            f"{info['seconds'] // 60}:{info['seconds'] % 60:02d}" if info["seconds"] else "",
            human(os.path.getsize(path)), "yes" if info["cover"] else "-", "ready"))
        self.tracks.append(t)

    def remove_selected(self):
        if self.busy:
            return
        sel = set(self.tree.selection())
        self.tracks = [t for t in self.tracks if t["iid"] not in sel]
        for iid in sel:
            self.tree.delete(iid)

    def clear_list(self):
        if self.busy:
            return
        self.tree.delete(*self.tree.get_children())
        self.tracks = []

    def _on_select(self, _e=None):
        sel = self.tree.selection()
        if not sel:
            return
        t = next((t for t in self.tracks if t["iid"] == sel[0]), None)
        if not t:
            return
        i = t["info"]
        self.detail_var.set(f"{i['title'] or t['stem']}\n{i['artist']}\n{i['album']}\n"
                            f"{i['bitrate']} kbps\nOn the iPod as:\n{t['name']}.mp3")
        try:
            if i["cover"]:
                from PIL import ImageTk
                img = M.blob_to_image(M.make_cover_blob(i["cover"], 0))
                self.preview_img = ImageTk.PhotoImage(img)
                self.cover_lbl.configure(image=self.preview_img, text="")
            else:
                self.cover_lbl.configure(image="", text="(no cover in this file)")
                self.preview_img = None
        except Exception as e:
            self.cover_lbl.configure(image="", text=f"(cover not readable: {e})")

    # ------------------------------------------------------------------ sending
    def _cancel(self):
        self.cancel_flag = True

    def need_download(self):
        """Queued YouTube songs whose MP3 is not on disk yet."""
        return [(i, t) for i, t in enumerate(self.tracks)
                if t.get("yt") and not (t["path"] and os.path.isfile(t["path"]))
                and not t["status"].startswith("done")]

    def _ffmpeg_check(self):
        if not YT.resolve_ffmpeg(self.cfg.get("ffmpeg")):
            messagebox.showerror(APP, "ffmpeg is missing - it converts YouTube audio to MP3.\n"
                                      "Install ffmpeg, run  pip install imageio-ffmpeg,  or use "
                                      "'Pick ffmpeg.exe...' on the YouTube tab.")
            return False
        return True

    def download_now(self):
        """Fetch + convert queued YouTube songs locally; the iPod is not needed."""
        need = self.need_download()
        if not need:
            messagebox.showinfo(APP, "Nothing to download - everything in the list is ready to send.")
            return
        if not self._ffmpeg_check():
            return
        self.cancel_flag = False

        def job():
            failed = self._dl_phase(need)
            msg = "Download finished - ready to be sent." if not failed else \
                  f"Download finished ({failed} song(s) failed - see the log)."
            self.q.put(("progress", 1, 1, 1, 1, msg))
            self.say(msg)
        self._run(job)

    def _dl_phase(self, need_dl):
        """Download + convert all given (index, track) pairs. Returns the indexes
        that failed. Raises P.Cancelled when the cancel flag is set."""
        failed = set()
        for n, (idx, t) in enumerate(need_dl, 1):
            if self.cancel_flag:
                raise P.Cancelled()
            r = t["yt"]
            label = r["song_title"] or r["title"]
            self.q.put(("row", idx, "downloading..."))

            def dprog(got, total, label=label, n=n, tot=len(need_dl)):
                frac = (got / total) if total else 0
                pct = 100 * ((n - 1) + frac) / tot
                text = f"downloading [{n}/{tot}] {label[:70]}   {human(got)} / " + (human(total) if total else "?")
                self.q.put(("ytprog", pct, text))
            try:
                self.say(f"Downloading [{n}/{len(need_dl)}] {label} at {self.yt_bitrate_var.get()} kbps...")
                path, _info = YT.download(
                    r["url"], bitrate=int(self.yt_bitrate_var.get()),
                    ffmpeg=self.cfg.get("ffmpeg"), cancel=lambda: self.cancel_flag,
                    progress=dprog, say=self.say)
                t["path"] = path          # worker-side first: send phase must not wait for the UI
                t["info"] = M.read_info(path)
                t["name"] = M.device_name(t["info"]["artist"], t["info"]["title"], t["stem"])
                self.q.put(("ytdone", idx, path))   # UI row refresh only
                self.q.put(("row", idx, "ready"))
            except P.Cancelled:
                raise
            except Exception as e:
                failed.add(idx)
                self.q.put(("row", idx, "FAILED"))
                self.say(f"  '{label}': download failed ({e})")
        return failed

    def send_all(self):
        if not self._need_dev():
            return
        todo = [(i, t) for i, t in enumerate(self.tracks) if not t["status"].startswith("done")]
        if not todo:
            messagebox.showinfo(APP, "Nothing to send - add some MP3 files first (or search YouTube).")
            return
        need_dl = [(i, t) for i, t in todo if t.get("yt") and not (t["path"] and os.path.isfile(t["path"]))]
        if need_dl and not self._ffmpeg_check():
            return
        self.cancel_flag = False
        strip, skip = self.strip_var.get(), self.skip_var.get()

        def job():
            dev = self.dev
            failed_idx = self._dl_phase(need_dl)      # phase 1: download + convert
            to_send, skipped = [], 0
            for i, t in todo:
                if i in failed_idx:
                    continue
                if t.get("yt") and not (t["path"] and os.path.isfile(t["path"])):
                    skipped += 1                      # belt & braces: never hand run_batch a missing file
                    self.q.put(("row", i, "FAILED"))
                    self.say(f"  '{t['name']}': file was not produced - skipped")
                    continue
                to_send.append((i, t))
            if not to_send:
                self.q.put(("progress", 1, 1, 1, 1, "No songs could be downloaded."))
                return
            sent, there, failed = run_batch(          # phase 2: transfer
                dev, to_send, strip, skip, lambda: self.cancel_flag,
                on_row=lambda i, s: self.q.put(("row", i, s)),
                on_progress=lambda *a: self.q.put(("progress",) + a),
                say=self.say)
            msg = f"Finished: {sent} sent, {there} already there, {failed} failed"
            if failed_idx:
                msg += f", {len(failed_idx)} download(s) failed"
            if skipped:
                msg += f", {skipped} skipped"
            self.q.put(("progress", 1, 1, 1, 1, msg))
            self.say(msg + ". Press 'Finish & start the player' when you are done.")
            self.q.put(("devlist", dev.list_songs()))
        self._run(job)

    # ------------------------------------------------------------------ iPod contents
    def refresh_device(self):
        if not self._need_dev():
            return
        self._run(lambda: self.q.put(("devlist", self.dev.list_songs())))

    def delete_selected(self):
        if not self._need_dev():
            return
        paths = [self.dev_tree.item(i, "values")[0] for i in self.dev_tree.selection()]
        if not paths:
            return
        if not messagebox.askyesno(APP, f"Delete {len(paths)} song(s) from the iPod's SD card?"):
            return

        def job():
            for p in paths:
                self.dev.delete(p)
                self.say(f"deleted {p}")
            self.q.put(("devlist", self.dev.list_songs()))
        self._run(job)

    def _close(self):
        if self.busy and not messagebox.askyesno(APP, "A transfer is running. Quit anyway?"):
            return
        self.cancel_flag = True
        if self.dev:
            try:
                self.dev.t.close()
            except Exception:
                pass
        self.destroy()


if __name__ == "__main__":
    App().mainloop()
