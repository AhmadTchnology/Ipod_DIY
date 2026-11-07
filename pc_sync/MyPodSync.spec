# -*- mode: python ; coding: utf-8 -*-
"""PyInstaller spec for MyPod Sync - one-file Windows exe, no install needed.

Build (from this pc_sync folder):
    py -m PyInstaller --clean --noconfirm MyPodSync.spec
Result: dist/MyPodSync.exe - copy that single file anywhere and run it.
It saves mypod_cfg.json + downloads/ next to wherever you put it.
"""
from PyInstaller.utils.hooks import collect_all

datas, binaries, hiddenimports = [], [], []
# yt_dlp: every module (extractors load dynamically) + data files.
# imageio_ffmpeg: pulls in the bundled ffmpeg binary as data, which the app
# finds again at sys._MEIPASS/imageio_ffmpeg/binaries (see resolve_ffmpeg).
# certifi: HTTPS certificates for the YouTube requests.
for pkg in ("yt_dlp", "imageio_ffmpeg", "certifi"):
    d, b, h = collect_all(pkg)
    datas += d
    binaries += b
    hiddenimports += h

a = Analysis(
    ["mypod_sync.py"],
    pathex=[],
    binaries=binaries,
    datas=datas,
    hiddenimports=hiddenimports,
    hookspath=[],
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name="MyPodSync",
    debug=False,
    strip=False,
    upx=False,
    console=False,
    icon=None,
)
