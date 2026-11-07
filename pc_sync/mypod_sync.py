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
from mypod_batch import run_batch, human

APP = "MyPod Sync"
SPEEDS = ["921600", "1500000", "2000000", "460800", "115200"]


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title(APP)
        self.geometry("1020x700")
        self.minsize(900, 600)
        self.q = queue.Queue()
        self.dev = None
        self.busy = False
        self.cancel_flag = False
        self.tracks = []              # dicts: path, info, name, status
        self.device_songs = {}        # dev path -> {size, cover}
        self.preview_img = None
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

        nb = ttk.Notebook(self)
        nb.pack(fill="both", expand=True, padx=8, pady=4)
        self.nb = nb

        # --- tab 1: songs to send
        t1 = ttk.Frame(nb, padding=6)
        nb.add(t1, text="Songs to send")
        bar = ttk.Frame(t1)
        bar.pack(fill="x")
        ttk.Button(bar, text="Add files...", command=self.add_files).pack(side="left")
        ttk.Button(bar, text="Add folder...", command=self.add_folder).pack(side="left", padx=4)
        ttk.Button(bar, text="Remove selected", command=self.remove_selected).pack(side="left")
        ttk.Button(bar, text="Clear list", command=self.clear_list).pack(side="left", padx=4)
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

        # --- tab 2: on the iPod
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

        # --- bottom
        bot = ttk.Frame(self, padding=8)
        bot.pack(fill="x")
        self.send_btn = ttk.Button(bot, text="Send to iPod", command=self.send_all)
        self.send_btn.pack(side="left")
        self.cancel_btn = ttk.Button(bot, text="Cancel", command=self._cancel, state="disabled")
        self.cancel_btn.pack(side="left", padx=4)
        self.exit_btn = ttk.Button(bot, text="Finish & start the player", command=self.exit_player)
        self.exit_btn.pack(side="right")
        pf = ttk.Frame(self, padding=(8, 0))
        pf.pack(fill="x")
        self.file_bar = ttk.Progressbar(pf, maximum=100)
        self.file_bar.pack(fill="x", pady=2)
        self.all_bar = ttk.Progressbar(pf, maximum=100)
        self.all_bar.pack(fill="x", pady=2)
        self.prog_var = tk.StringVar()
        ttk.Label(pf, textvariable=self.prog_var, foreground="#555").pack(anchor="w")
        self.log = tk.Text(self, height=6, state="disabled", background="#f6f6f6")
        self.log.pack(fill="x", padx=8, pady=(4, 8))

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

    def send_all(self):
        if not self._need_dev():
            return
        todo = [(i, t) for i, t in enumerate(self.tracks) if not t["status"].startswith("done")]
        if not todo:
            messagebox.showinfo(APP, "Nothing to send - add some MP3 files first.")
            return
        self.cancel_flag = False
        strip, skip = self.strip_var.get(), self.skip_var.get()

        def job():
            dev = self.dev
            sent, there, failed = run_batch(
                dev, todo, strip, skip, lambda: self.cancel_flag,
                on_row=lambda i, s: self.q.put(("row", i, s)),
                on_progress=lambda *a: self.q.put(("progress",) + a),
                say=self.say)
            msg = f"Finished: {sent} sent, {there} already there, {failed} failed"
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
