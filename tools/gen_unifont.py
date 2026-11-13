"""Generates src/unifont_data.h (Unicode bitmap font + Arabic shaping tables for MyPod)
and a preview PNG that simulates the pod's renderer with the generated bitmaps.
Only needs PIL + arabic-reshaper (validation) + fontTools (coverage checks)."""
import json, math, os, sys
from PIL import Image, ImageDraw, ImageFont

TEMP = os.path.join(os.environ["TEMP"], "opencode")
OUT_H = r"E:\IDM Download\Ipod CYD\Ipod_DIY_v1.1_USB_sync\Ipod_DIY_v1.1\src\unifont_data.h"
INV_PATH = os.path.join(TEMP, "tag_inventory.json")
H_BIG, H_SMALL = 19, 12   # BIG 19px -> Arial 16, cap ~11 = built-in font2 cap height
FONTS = {  # per-script primary sources (Arial matches the built-in pixel font size/weight best)
    "latin": r"C:\Windows\Fonts\arial.ttf",
    "cjk":   r"C:\Windows\Fonts\msyhl.ttc",
    "hangul": r"C:\Windows\Fonts\malgun.ttf",
}

# ------------------------------------------------------------------ charsets
def build_charset():
    s = set(range(0x20, 0x7F))
    s |= set(range(0xA0, 0x100))                    # Latin-1
    s |= set(range(0x100, 0x180))                   # Latin Ext-A
    s |= set(range(0x37F, 0x400))                   # Greek
    s |= set(range(0x400, 0x460))                   # Cyrillic
    s |= {0x2010, 0x2011, 0x2013, 0x2014, 0x2018, 0x2019, 0x201A, 0x201C, 0x201D, 0x201E,
          0x2020, 0x2021, 0x2022, 0x2026, 0x2030, 0x2032, 0x2033, 0x2039, 0x203A,
          0x20AC, 0x2122, 0xA9, 0xAE, 0xB0, 0xB7, 0xAB, 0xBB, 0x202F, 0x200E, 0x200F}
    s |= {0x060C, 0x061B, 0x061F}
    s |= set(range(0x0621, 0x064B))                 # Arabic letters (no harakat)
    s |= {0x0640}                                   # tatweel
    s |= set(range(0x0660, 0x066E))                 # Arabic-Indic digits + separators
    s |= {0x0671, 0x067E, 0x0686, 0x0698, 0x06A4, 0x06A9, 0x06AF, 0x06C1, 0x06BE, 0x06CC, 0x06D2}
    s |= set(range(0xFE70, 0xFEFD))                 # presentation forms B (+ lam-alef)
    s |= {0xFB50, 0xFB51, 0xFB56, 0xFB57, 0xFB58, 0xFB59, 0xFB6A, 0xFB6B, 0xFB6C, 0xFB6D,
          0xFB7A, 0xFB7B, 0xFB7C, 0xFB7D, 0xFB8A, 0xFB8B, 0xFB8E, 0xFB8F, 0xFB90, 0xFB91,
          0xFB92, 0xFB93, 0xFB94, 0xFB95, 0xFBFC, 0xFBFD, 0xFBFE}   # presentation forms A
    # CJK / Hangul subset = exactly what the library's tags use (tag_inventory.json,
    # produced by the tag scan). Optional: without it the atlas has no CJK glyphs.
    if os.path.exists(INV_PATH):
        inv = json.load(open(INV_PATH, encoding="utf-8"))
        for c in inv["all_nonascii"]:
            o = ord(c)
            if 0x2E80 <= o <= 0x9FFF or 0xAC00 <= o <= 0xD7A3 or 0xF900 <= o <= 0xFAFF or 0xFF00 <= o <= 0xFFEF:
                s.add(o)
    return sorted(s)

CHARSET = build_charset()

# ------------------------------------------------------------------ arabic shaping tables
import arabic_reshaper
RS = arabic_reshaper.ArabicReshaper(configuration={
    "delete_harakat": True, "delete_tatweel": False, "support_ligatures": True})

BEH, LAM = "\u0628", "\u0644"

def _forms(c):
    """Probe arabic_reshaper for the (iso, fin, ini, med) presentation forms of letter c.
    Context = beh, which offers a left connection in every probe position (tatweel
    would make the reshaper emit a second codepoint and break the len==1 checks)."""
    iso_o = RS.reshape(c)
    fin_o = RS.reshape(BEH + c)          # c connected to previous -> final form
    ini_o = RS.reshape(c + BEH)          # c offering connection -> initial form
    med_o = RS.reshape(BEH + c + BEH)    # both sides -> medial form
    iso = ord(iso_o) if len(iso_o) == 1 else 0
    fin = ord(fin_o[-1]) if fin_o else 0
    ini = ord(ini_o[0]) if ini_o else 0
    med = ord(med_o[1]) if len(med_o) >= 2 else 0
    if fin == iso: fin = 0               # U-type letters probe back their isolated form
    if ini == iso: ini = 0
    if med == iso: med = 0
    return iso, fin, ini, med

JOIN_TABLE = []      # (cp, connects_right, connects_left, iso, fin, ini, med)
LAM_ALEF = {}        # alef_cp -> (iso_lig, fin_lig)
EXTRA_FORMS = set()

for cp in sorted(CHARSET):
    c = chr(cp)
    if not (0x0600 <= cp <= 0x06FF or cp == 0x0640):
        continue
    iso, fin, ini, med = _forms(c)
    if not iso:
        continue
    EXTRA_FORMS |= {x for x in (iso, fin, ini, med) if x}
    connects_right = bool(fin)             # letter takes a form connected to the previous char
    connects_left = bool(ini)              # letter offers a connection to the next char
    if cp == 0x0640:                       # tatweel joins on both sides
        connects_right = connects_left = True
        iso = fin = ini = med = cp
    JOIN_TABLE.append((cp, int(connects_right), int(connects_left), iso, fin, ini, med))

# lam + X ligatures: every letter the reshaper collapses into one codepoint after lam
for cp in [c for c in sorted(CHARSET) if 0x0621 <= c <= 0x06FF]:
    out = RS.reshape(LAM + chr(cp))
    if len(out) == 1 and ord(out) != cp:
        fin_o = RS.reshape(BEH + LAM + chr(cp))       # connected ligature = final form
        LAM_ALEF[cp] = {"iso": ord(out), "fin": ord(fin_o[-1]) if fin_o else ord(out)}
        EXTRA_FORMS |= {LAM_ALEF[cp]["iso"], LAM_ALEF[cp]["fin"]}

EXTRA_FORMS.add(0xFDF2)                   # the Allah word ligature (Arial has it)

print(f"join table: {len(JOIN_TABLE)} letters; lam-alef: "
      + ", ".join(f"{a:04X}:({LAM_ALEF[a]['iso']:04X},{LAM_ALEF[a]['fin']:04X})" for a in LAM_ALEF))

# ------------------------------------------------------------------ the renderer algorithm (mirrors the C code)
HARAKAT = set(range(0x064B, 0x0653)) | set(range(0x0653, 0x0656)) | {0x0670} | \
          set(range(0x06D6, 0x06DD)) | set(range(0x06DF, 0x06E5)) | set(range(0x06E7, 0x06E9)) | \
          set(range(0x06EA, 0x06ED)) | {0x200E, 0x200F, 0x202A, 0x202B, 0x202C, 0x202D, 0x202E}

def joins_tbl():
    return {cp: (r, l, iso, fin, ini, med) for cp, r, l, iso, fin, ini, med in JOIN_TABLE}

TBL = joins_tbl()

# Glyphs that must keep their natural advance: Arabic letters and every shaped
# form they can produce (isolated/final/initial/medial, lam-alef, Allah) - the
# connected strokes rely on the font's own advance to touch the neighbour.
NO_SPACE = set(TBL.keys())
for _cp, _r, _l, _iso, _fin, _ini, _med in JOIN_TABLE:
    NO_SPACE |= {x for x in (_iso, _fin, _ini, _med) if x}
for _a in LAM_ALEF.values():
    NO_SPACE |= {_a["iso"], _a["fin"]}
NO_SPACE |= {0xFDF2, 0x0640}

def _class(cp):
    if cp in HARAKAT:
        return None
    if 0x0600 <= cp <= 0x06FF or 0xFB50 <= cp <= 0xFEFF or cp in TBL:
        return "AN" if 0x0660 <= cp <= 0x0669 else "R"   # digits read LTR inside RTL
    if cp < 0x80 or 0x80 <= cp <= 0x24F or 0x370 <= cp <= 0x3FF or 0x400 <= cp <= 0x4FF \
            or 0x2E80 <= cp <= 0x9FFF or 0xAC00 <= cp <= 0xD7A3 or 0xF900 <= cp <= 0xFAFF \
            or 0xFF00 <= cp <= 0xFFEF:
        return "L"
    return "N"

def shape(s):
    """Logical codepoints -> shaped, harakat dropped (firmware does the same)."""
    out = []
    prev_left = False                     # does the previous letter offer a connection to us?
    n = len(s)
    i = 0
    while i < n:
        cp = ord(s[i])
        i += 1
        if cp in HARAKAT:
            continue                      # zero-width, joining stays transparent
        if cp == 0x0627 and not prev_left:      # the Allah word: ا ل ل ه -> U+FDF2
            j = i
            while j < n and ord(s[j]) in HARAKAT:
                j += 1
            if j + 1 < n and ord(s[j]) == 0x0644 and ord(s[j + 1]) == 0x0644:
                k = j + 2
                while k < n and ord(s[k]) in HARAKAT:
                    k += 1
                if k < n and ord(s[k]) == 0x0647:
                    m = k + 1
                    while m < n and ord(s[m]) in HARAKAT:
                        m += 1
                    ne = TBL.get(ord(s[m])) if m < n else None
                    if not (ne and ne[0]):          # nothing may join to the final heh
                        out.append(0xFDF2)
                        prev_left = False
                        i = m
                        continue
        if cp == 0x0644:                  # lam: ligature with the next alef (harakat between skipped)
            j = i
            while j < n and ord(s[j]) in HARAKAT:
                j += 1
            if j < n and ord(s[j]) in LAM_ALEF:
                lig = LAM_ALEF[ord(s[j])]
                out.append(lig["fin"] if prev_left else (lig["iso"] or lig["fin"]))
                prev_left = False         # the ligature glyph joins to nothing on its left
                i = j + 1
                continue
        e = TBL.get(cp)
        if e is None:                     # not an Arabic letter: pass through
            out.append(cp)
            prev_left = (cp == 0x0640)    # only tatweel connects
            continue
        r, cl, iso, fin, ini, med = e
        j = i
        while j < n and ord(s[j]) in HARAKAT:
            j += 1
        next_conn = False                 # does the next letter accept a connection?
        if j < n:
            ne = TBL.get(ord(s[j]))
            next_conn = bool(ne[0]) if ne else False
        if prev_left:
            out.append(med if (next_conn and cl and med) else (fin or iso))
        else:
            out.append(ini if (next_conn and ini) else iso)
        prev_left = bool(cl)
    return out

def bidi(seq):
    """Simple bidi: base = first strong class; RTL base reverses all, LTR runs restored."""
    strong = ("R", "L")
    base = next((c for c in (_class(x) for x in seq) if c in strong), "L")
    if base == "R":
        vis = list(reversed(seq))
        i = 0
        while i < len(vis):                       # restore LTR runs (letters + digits)
            if _class(vis[i]) in ("L", "AN"):
                j = i
                while j < len(vis) and _class(vis[j]) in ("L", "AN"):
                    j += 1
                vis[i:j] = vis[i:j][::-1]
                i = j
            else:
                i += 1
        return vis
    vis, i = list(seq), 0
    while i < len(vis):                           # reverse RTL runs in place
        if _class(vis[i]) == "R":
            j = i
            while j < len(vis) and _class(vis[j]) == "R":
                j += 1
            vis[i:j] = vis[i:j][::-1]
            i = j
        else:
            i += 1
    return vis

def visual(s):
    return bidi(shape(s))

# ------------------------------------------------------------------ validate against arabic_reshaper
if os.path.exists(INV_PATH):
    inv = json.load(open(INV_PATH, encoding="utf-8"))
    corpus = [e["title"] for e in inv["tagged"] if e["title"]] + [e["artist"] for e in inv["tagged"] if e["artist"]]
    corpus += ["بلا", "لأ", "لآ", "لإ", "الله", "لا", "ــلا", "بالله", "Test 123 لا", "السلام عليكم", "برقين لاح"]
    bad = 0
    for s in corpus:
        mine = "".join(chr(c) for c in shape(s) if c)
        ref = RS.reshape(s)
        if mine != ref:
            bad += 1
            print(f"SHAPE MISMATCH [{s}]")
            print(f"  mine: {mine}")
            print(f"  ref : {ref}")
    print(f"shaping validation: {len(corpus) - bad}/{len(corpus)} identical to arabic_reshaper")
    if bad:
        sys.exit("shaping validation failed - fix the algorithm before emitting")

# ------------------------------------------------------------------ rasterization
def pick_size(path, target_h):
    """Largest font size whose ascent+descent <= target_h."""
    best = (None, -1)
    for size in range(6, 24):
        f = ImageFont.truetype(path, size)
        a, d = f.getmetrics()
        if a + d <= target_h and a + d > best[1]:
            best = (size, a + d)
    return best[0]

def font_for(cp):
    if 0xAC00 <= cp <= 0xD7A3:
        return FONTS["hangul"]
    if 0x2E80 <= cp <= 0x9FFF or 0xF900 <= cp <= 0xFAFF or 0xFF00 <= cp <= 0xFFEF:
        return FONTS["cjk"]
    return FONTS["latin"]

def rasterize(path, cps, atlas_h, label):
    size = pick_size(path, atlas_h)
    f = ImageFont.truetype(path, size)
    asc, desc = f.getmetrics()
    print(f"{label}: font size {size}, line {asc}+{desc}={asc+desc} <= {atlas_h}")
    glyphs = {}
    clipped = over_bot = over_top = 0
    for cp in cps:
        ch = chr(cp)
        if cp in HARAKAT:
            continue
        adv = int(round(f.getlength(ch)))
        if adv < 1:
            adv = 1
        if cp not in NO_SPACE:
            adv += 1                    # tracking: open up letter spacing (Arabic joins stay untouched)
        bb = f.getbbox(ch)                      # ink box relative to pen origin (anchor la)
        if bb is None or bb[2] <= bb[0] or bb[3] <= bb[1]:
            glyphs[cp] = (adv, 0, 0, 0, 0, b"") # advance-only glyph (space etc.)
            continue
        x0, y0, x1, y1 = bb
        w, h = x1 - x0, y1 - y0
        im = Image.new("L", (w, h), 0)
        ImageDraw.Draw(im).text((-x0, -y0), ch, font=f, fill=255, anchor="la")
        # Clip the ink to the atlas line band: TrueType descenders/parens can
        # reach below the descent metric, and any ink outside [0, atlas_h) is
        # never covered by the UI's line-band clear -> leftover pixel slivers.
        top = max(0, -y0)
        bot = min(h, atlas_h - y0)
        if bot - top < h:
            clipped += 1
            over_bot = max(over_bot, max(0, y0 + h - atlas_h))
            over_top = max(over_top, max(0, -y0))
        if bot <= top:
            glyphs[cp] = (adv, 0, 0, 0, 0, b"")
            continue
        if top or bot < h:
            im = im.crop((0, top, w, bot))
            y0 += top
            h = bot - top
        glyphs[cp] = (adv, w, x0, h, y0, im.tobytes())
    if clipped:
        print(f"  {label}: clipped {clipped} glyphs to the line band "
              f"(max {over_bot}px below, {over_top}px above)")
    return glyphs, asc

cmap_cache = {}
def covered(cp):
    path = font_for(cp)
    from fontTools.ttLib import TTFont
    if path not in cmap_cache:
        t = TTFont(path, fontNumber=0, lazy=True)
        cmap_cache[path] = set(t.getBestCmap())
        t.close()
    return path if cp in cmap_cache[path] else None

# assemble the final glyph set: charset + every form the join table can emit
all_cps = set(CHARSET) | EXTRA_FORMS
all_cps = {c for c in all_cps if c not in HARAKAT}
missing = [c for c in sorted(all_cps) if not covered(c)]
if missing:
    print("missing glyphs (will render as '?'):", ", ".join(f"{c:04X}" for c in missing))
    all_cps -= set(missing)

big, small = {}, {}
g_big, asc_big = rasterize(FONTS["latin"], sorted(c for c in all_cps if font_for(c) == FONTS["latin"]), H_BIG, "BIG")
g_small, asc_small = rasterize(FONTS["latin"], sorted(c for c in all_cps if font_for(c) == FONTS["latin"]), H_SMALL, "SMALL")
big.update(g_big); small.update(g_small)
for other in (FONTS["cjk"], FONTS["hangul"]):
    cps = sorted(c for c in all_cps if font_for(c) == other)
    if not cps:
        continue
    for target, dst in ((H_BIG, big), (H_SMALL, small)):
        gg, _ = rasterize(other, cps, target, f"{os.path.basename(other)} {target}px")
        dst.update(gg)

def build_atlas(glyphs):
    idx, data = [], bytearray()
    for cp in sorted(glyphs):
        adv, w, xoff, h, yoff, rows = glyphs[cp]
        stride = (w + 7) // 8
        packed = bytearray()
        for y in range(h):
            row = rows[y * w:(y + 1) * w]
            line = bytearray(stride)
            for x in range(w):
                if row[x]:
                    line[x >> 3] |= 0x80 >> (x & 7)
            packed += line
        idx.append((cp, len(data)))
        data += bytes([adv & 0xFF, w & 0xFF, xoff & 0xFF, h & 0xFF, yoff & 0xFF]) + bytes(packed)
    return idx, bytes(data)

idx_big, data_big = build_atlas(big)
idx_small, data_small = build_atlas(small)
print(f"atlas BIG: {len(big)} glyphs, {len(data_big)} bytes | SMALL: {len(small)} glyphs, {len(data_small)} bytes")

# ------------------------------------------------------------------ emit C header
def c_bytes(b, per=20):
    lines = []
    for i in range(0, len(b), per):
        lines.append("  " + ",".join(f"0x{x:02X}" for x in b[i:i + per]) + ("," if i + per < len(b) else ""))
    return "\n".join(lines)

def c_idx(idx):
    lines = []
    for cp, off in idx:
        lines.append(f"  {{{cp:#06x},{off}}},")
    return "\n".join(lines)

join_lines = "\n".join(
    "  {%s,%d,%s,%s,%s,%s}," % (f"{cp:#06x}", r | (l << 1), f"{iso:#06x}", f"{fin:#06x}",
                                f"{ini:#06x}", f"{med:#06x}")
    for cp, r, l, iso, fin, ini, med in JOIN_TABLE)
lam_alef_line = ",".join(f"{a:#06x}" for a in sorted(LAM_ALEF))
lam_iso_line = ",".join(f"{LAM_ALEF[a]['iso']:#06x}" for a in sorted(LAM_ALEF))
lam_fin_line = ",".join(f"{LAM_ALEF[a]['fin']:#06x}" for a in sorted(LAM_ALEF))

hdr = """// unifont_data.h - GENERATED by tools/gen_unifont.py (do not edit by hand).
// Unicode bitmap atlas for song names/tags: Latin + Greek + Cyrillic + Arabic
// (with presentation forms for the shaping table) + the CJK/Hangul chars found
// in this library's tags. Fonts: Arial / Microsoft YaHei UI Light / Malgun Gothic.
// Record layout in data[]: u8 adv (pen advance), u8 ink_w, i8 ink_xoff,
// u8 ink_h, i8 ink_yoff, then ink_h*(ink_w+7)/8 packed rows (MSB = leftmost
// pixel). Draw ink at (pen+ink_xoff, line_top+ink_yoff), advance pen by adv.
// Index is sorted by codepoint.
#pragma once
#include <stdint.h>

namespace UniFont {

static const uint8_t H_BIG = %d;
static const uint8_t H_SMALL = %d;

struct Rec { uint16_t cp; uint16_t off; };
struct Join { uint16_t cp; uint8_t flags; uint16_t iso, fin, ini, med; };

static const Rec idxBig[] = {
%s
};
static const uint8_t dataBig[] = {
%s
};
static const int NB = %d;

static const Rec idxSmall[] = {
%s
};
static const uint8_t dataSmall[] = {
%s
};
static const int NS = %d;

// flags: bit0 = letter connects to the previous char (right side joins),
//        bit1 = letter offers a connection to the next char (left side joins).
static const Join joins[] = {
%s
};
static const int NJ = %d;

// lam(0644) + alef ligatures: [alef cp] = {isolated form, final form}
static const uint16_t lamAlefAlef[] = {%s};
static const uint16_t lamAlefIso[]  = {%s};
static const uint16_t lamAlefFin[]  = {%s};
static const int NLA = %d;

}  // namespace UniFont
""" % (H_BIG, H_SMALL, c_idx(idx_big), c_bytes(data_big), len(idx_big),
       c_idx(idx_small), c_bytes(data_small), len(idx_small),
       join_lines, len(JOIN_TABLE), lam_alef_line, lam_iso_line, lam_fin_line, len(LAM_ALEF))
os.makedirs(os.path.dirname(OUT_H), exist_ok=True)
open(OUT_H, "w", encoding="utf-8", newline="\n").write(hdr)
print("wrote", OUT_H)

# ------------------------------------------------------------------ preview: mock pod screens from the generated bitmaps (optional)
if os.path.exists(INV_PATH):
    inv = json.load(open(INV_PATH, encoding="utf-8"))
    def glyph(cp, style):
        src = big if style == H_BIG else small
        idx, data = (idx_big, data_big) if style == H_BIG else (idx_small, data_small)
        lo, hi = 0, len(idx) - 1
        while lo <= hi:
            mid = (lo + hi) // 2
            if idx[mid][0] < cp:
                lo = mid + 1
            elif idx[mid][0] > cp:
                hi = mid - 1
            else:
                off = idx[mid][1]
                adv, w, xoff, h, yoff = data[off:off + 5]
                xoff = xoff - 256 if xoff > 127 else xoff      # signed bytes
                yoff = yoff - 256 if yoff > 127 else yoff
                stride = (w + 7) // 8
                rows = data[off + 5:off + 5 + h * stride]
                return adv, w, xoff, h, yoff, rows, stride
        q = big.get(0x3F)
        if not q:
            return None
        return glyph(0x3F, style)

    def draw_text(img, dr, seq, x, y, style, fg, bg):
        for cp in seq:
            g = glyph(cp, style)
            if g is None:
                dr.rectangle([x, y, x + 7, y + style - 1], fill=bg, outline=fg)
                x += 8
                continue
            adv, w, xoff, h, yoff, rows, stride = g
            for yy in range(h):
                for xx in range(w):
                    px = x + xoff + xx, y + yoff + yy
                    if 0 <= px[0] < img.width and 0 <= px[1] < img.height:
                        bit = rows[yy * stride + (xx >> 3)] & (0x80 >> (xx & 7))
                        dr.point(px, fill=fg if bit else bg)
            x += adv
        return x

    BG, TXT, DIM, ACC = (18, 20, 26), (235, 235, 235), (140, 150, 165), (255, 140, 60)

    songs = [(e["artist"], e["title"]) for e in inv["tagged"] if e["title"]]
    n = len(songs)
    cell_w, cell_h = 250, 320
    sheet = Image.new("RGB", (n * cell_w + 10, cell_h + 10), (60, 60, 60))
    dr = ImageDraw.Draw(sheet)
    for i, (artist, title) in enumerate(songs):
        ox = 5 + i * cell_w
        dr.rectangle([ox, 5, ox + 240, 325], fill=BG, outline=(90, 90, 90))
        t = visual(title)[:40]
        a = visual(artist)[:40]
        draw_text(sheet, dr, t, ox + 20, 230, H_BIG, TXT, BG)
        draw_text(sheet, dr, a, ox + 20, 252, H_SMALL, DIM, BG)
        dr.rectangle([ox + 19, 274, ox + 220, 282], outline=(120, 120, 120))
        dr.rectangle([ox + 20, 275, ox + 20 + 120, 281], fill=ACC)
        dr.line([ox + 8, 220, ox + 232, 220], fill=(70, 75, 85))
    out = os.path.join(TEMP, "uniname_previews.png")
    sheet.save(out)
    print("preview:", out)
    json.dump({"big": len(big), "small": len(small), "bytes": len(data_big) + len(data_small),
               "missing": missing}, open(os.path.join(TEMP, "uni_stats.json"), "w"))
