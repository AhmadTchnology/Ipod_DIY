// unitext.h - Unicode text rendering for MyPod.
//
// Everything the built-in ASCII fonts can't do: UTF-8 decoding, Arabic shaping
// (contextual forms + lam-alef + the Allah ligature, validated against
// arabic-reshaper), simple bidi (base direction from the first strong char,
// LTR runs kept in order) and bitmap drawing from the generated atlas in
// unifont_data.h. Glyph records: u8 adv, u8 ink_w, i8 ink_xoff, u8 ink_h,
// i8 ink_yoff, packed MSB-left rows - the same layout TFT_eSPI::drawBitmap
// expects (128 >> bit).
//
// Missing glyphs render as '?' (always present in the atlas).
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>
#include "unifont_data.h"

namespace Uni {

inline bool hasUni(const char* s) {
  for (; *s; s++) if ((uint8_t)*s >= 0x80) return true;
  return false;
}

// pixel height of the line for a ui.cpp font/size combo
inline int lineH(uint8_t font, uint8_t size) {
  return (font == 2 || size > 1) ? UniFont::H_BIG : UniFont::H_SMALL;
}

// ---------------------------------------------------------------- tables
inline const UniFont::Join* joinOf(uint16_t cp) {
  for (int i = 0; i < UniFont::NJ; i++)
    if (UniFont::joins[i].cp == cp) return &UniFont::joins[i];
  return nullptr;
}

inline int lamAlefIdx(uint16_t cp) {
  for (int i = 0; i < UniFont::NLA; i++)
    if (UniFont::lamAlefAlef[i] == cp) return i;
  return -1;
}

// combining marks + invisible marks: dropped at render (joining stays transparent)
inline bool harakat(uint16_t cp) {
  return (cp >= 0x064B && cp <= 0x0655) || cp == 0x0670 ||
         (cp >= 0x06D6 && cp <= 0x06DC) || (cp >= 0x06DF && cp <= 0x06E4) ||
         (cp >= 0x06E7 && cp <= 0x06E8) || (cp >= 0x06EA && cp <= 0x06EC) ||
         cp == 0x200E || cp == 0x200F || (cp >= 0x202A && cp <= 0x202E);
}

// ---------------------------------------------------------------- bidi classes
// 0 = L (strong left-to-right), 1 = R (Arabic), 2 = AN (Arabic digits, LTR run), 3 = neutral
inline uint8_t clsOf(uint16_t cp) {
  if ((cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0xFB50 && cp <= 0xFEFF) || joinOf(cp))
    return (cp >= 0x0660 && cp <= 0x0669) ? 2 : 1;
  if (cp < 0x80 || (cp >= 0x80 && cp <= 0x24F) || (cp >= 0x370 && cp <= 0x3FF) ||
      (cp >= 0x400 && cp <= 0x4FF) || (cp >= 0x2E80 && cp <= 0x9FFF) ||
      (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
      (cp >= 0xFF00 && cp <= 0xFFEF))
    return 0;
  return 3;
}

// ---------------------------------------------------------------- shaping
// UTF-8 string -> visual-order codepoints. Returns the count (<= maxCp).
// Mirrors tools/gen_unifont.py: same joining algorithm, harakat dropped.
inline int visual(const char* s, uint16_t* out, int maxCp) {
  if (maxCp > 128) maxCp = 128;
  uint16_t cps[128];
  int nc = 0;
  size_t n = strlen(s);
  for (size_t i = 0; i < n && nc < 128; ) {
    uint8_t c = (uint8_t)s[i];
    if (c < 0x80) { cps[nc++] = c; i++; continue; }
    int extra = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 0;
    bool ok = extra > 0 && i + extra < n;
    uint32_t cp = extra ? (c & (extra == 1 ? 0x1F : extra == 2 ? 0x0F : 0x07)) : 0;
    for (int k = 1; ok && k <= extra; k++) {
      uint8_t cc = (uint8_t)s[i + k];
      if ((cc & 0xC0) != 0x80) ok = false;
      else cp = (cp << 6) | (cc & 0x3F);
    }
    if (!ok) { cps[nc++] = '?'; i++; }
    else {
      if (cp == 0x2010 || cp == 0x2011) cp = '-';   // Arial atlas lacks these hyphens
      cps[nc++] = (uint16_t)cp; i += extra + 1;
    }
  }

  int no = 0;
  bool prevLeft = false;                // previous letter offers a connection to us
  for (int i = 0; i < nc && no < maxCp; i++) {
    uint16_t cp = cps[i];
    if (harakat(cp)) continue;
    if (cp == 0x0627 && !prevLeft) {    // the Allah word: ا ل ل ه -> U+FDF2
      int j = i + 1;
      while (j < nc && harakat(cps[j])) j++;
      if (j + 1 < nc && cps[j] == 0x0644 && cps[j + 1] == 0x0644) {
        int k = j + 2;
        while (k < nc && harakat(cps[k])) k++;
        if (k < nc && cps[k] == 0x0647) {
          int m = k + 1;
          while (m < nc && harakat(cps[m])) m++;
          const UniFont::Join* ne = m < nc ? joinOf(cps[m]) : nullptr;
          if (!(ne && (ne->flags & 1))) {           // nothing may join to the final heh
            out[no++] = 0xFDF2;
            prevLeft = false;
            i = m - 1;
            continue;
          }
        }
      }
    }
    if (cp == 0x0644) {                 // lam + alef -> mandatory ligature
      int j = i + 1;
      while (j < nc && harakat(cps[j])) j++;
      if (j < nc) {
        int la = lamAlefIdx(cps[j]);
        if (la >= 0) {
          out[no++] = prevLeft ? UniFont::lamAlefFin[la] : UniFont::lamAlefIso[la];
          prevLeft = false;             // the ligature glyph joins to nothing on its left
          i = j;
          continue;
        }
      }
    }
    const UniFont::Join* e = joinOf(cp);
    if (!e) {                           // not an Arabic letter: pass through
      out[no++] = cp;
      prevLeft = (cp == 0x0640);        // only tatweel connects
      continue;
    }
    int j = i + 1;
    while (j < nc && harakat(cps[j])) j++;
    bool nextConn = false;              // does the next letter accept a connection?
    if (j < nc) {
      const UniFont::Join* ne = joinOf(cps[j]);
      nextConn = ne && (ne->flags & 1);
    }
    bool cl = (e->flags & 2) != 0;      // we offer a connection to the next letter
    if (prevLeft) out[no++] = (nextConn && cl && e->med) ? e->med : (e->fin ? e->fin : e->iso);
    else          out[no++] = (nextConn && e->ini) ? e->ini : e->iso;
    prevLeft = cl;
  }

  // ---- bidi: base direction from the first strong char; RTL reverses all,
  //      LTR runs (letters + Arabic digits) are restored, AN stays LTR.
  uint8_t base = 0;                     // 0 = L
  for (int i = 0; i < no; i++) {
    uint8_t c = clsOf(out[i]);
    if (c == 0 || c == 1) { base = c; break; }
  }
  if (base == 1) {                      // RTL base: reverse everything, restore LTR runs
    for (int a = 0, b = no - 1; a < b; a++, b--) { uint16_t t = out[a]; out[a] = out[b]; out[b] = t; }
    for (int i = 0; i < no; ) {
      uint8_t c = clsOf(out[i]);
      if (c == 0 || c == 2) {
        int j = i;
        while (j < no && ((c = clsOf(out[j])) == 0 || c == 2)) j++;
        for (int a = i, b = j - 1; a < b; a++, b--) { uint16_t t = out[a]; out[a] = out[b]; out[b] = t; }
        i = j;
      } else i++;
    }
  } else {                              // LTR base: reverse RTL runs in place
    for (int i = 0; i < no; ) {
      if (clsOf(out[i]) == 1) {
        int j = i;
        while (j < no && clsOf(out[j]) == 1) j++;
        for (int a = i, b = j - 1; a < b; a++, b--) { uint16_t t = out[a]; out[a] = out[b]; out[b] = t; }
        i = j;
      } else i++;
    }
  }
  return no;
}

// ---------------------------------------------------------------- glyphs
inline const uint8_t* glyph(uint16_t cp, int h) {
  const bool big = h >= UniFont::H_BIG;
  const UniFont::Rec* idx = big ? UniFont::idxBig : UniFont::idxSmall;
  const uint8_t* data = big ? UniFont::dataBig : UniFont::dataSmall;
  int n = big ? UniFont::NB : UniFont::NS;
  int lo = 0, hi = n - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (idx[mid].cp < cp) lo = mid + 1;
    else if (idx[mid].cp > cp) hi = mid - 1;
    else return data + idx[mid].off;
  }
  return glyph(0x3F, h);                // missing glyph -> '?'
}

inline int width(const char* s, int h) {
  uint16_t vis[128];
  int no = visual(s, vis, 128);
  int w = 0;
  for (int i = 0; i < no; i++) w += glyph(vis[i], h)[0];
  return w;
}

// Draw one line with its top-left at (x, y), clipped horizontally to [cx0, cx1).
// The caller paints the line background; only ink pixels are drawn (fg).
inline void draw(TFT_eSPI& t, const char* s, int x, int y, int h, uint16_t fg,
                 int cx0 = 0, int cx1 = 320) {
  uint16_t vis[128];
  int no = visual(s, vis, 128);
  int pen = x;
  for (int i = 0; i < no; i++) {
    const uint8_t* g = glyph(vis[i], h);
    uint8_t adv = g[0], w = g[1], gh = g[3];
    int8_t xoff = (int8_t)g[2], yoff = (int8_t)g[4];
    int gx = pen + xoff;
    if (gh && gx < cx1 && gx + w > cx0 && y + yoff >= 0 && y + yoff + gh <= 320) {
      if (gx >= cx0 && gx + w <= cx1) {
        t.drawBitmap((int16_t)gx, (int16_t)(y + yoff), g + 5, w, gh, fg);
      } else {                          // glyph straddles the clip edge: shift bits
        int x0 = gx < cx0 ? cx0 : gx;
        int x1 = gx + w > cx1 ? cx1 : gx + w;
        int vw = x1 - x0, sh = x0 - gx;
        int st = (w + 7) / 8, vt = (vw + 7) / 8;
        uint8_t tmp[20 * 3];
        for (int yy = 0; yy < gh && yy < 20; yy++) {
          const uint8_t* row = g + 5 + (size_t)yy * st;
          for (int b = 0; b < vt; b++) {
            int bit = sh + b * 8, by = bit >> 3;
            uint8_t v = 0;
            if (by < st) {
              v = (uint8_t)(row[by] << (bit & 7));
              if ((bit & 7) && by + 1 < st) v |= (uint8_t)(row[by + 1] >> (8 - (bit & 7)));
            }
            tmp[yy * vt + b] = v;
          }
        }
        t.drawBitmap((int16_t)x0, (int16_t)(y + yoff), tmp, vw, gh, fg);
      }
    }
    pen += adv;
  }
}

}  // namespace Uni
