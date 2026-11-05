// =====================================================================
//  WiFi receiver — a separate boot mode, chosen in Settings.
//
//  Why a separate mode: the normal player leaves ~41 KB of heap free
//  (LVGL + Bluetooth + Helix). TLS (Telegram's HTTPS) needs ~50 KB of
//  its own, so it can only run when LVGL and Bluetooth are out of the
//  picture. Receiver mode therefore never starts either of them.
//
//  Flow:
//    1. softAP "MyPod-XXXX" (pass 12345678) + web portal at
//       http://192.168.4.1  ->  home SSID/password + bot token (NVS).
//    2. A task on core 0 connects to the home network and long-polls
//       your bot; audio/mp3 documents are saved to /music/telegram
//       (only the attachment's own file_id, never its cover thumbnail).
//    3. Exit: /exit in the browser, or hold BOOT for 3 s. Normal
//       player resumes (received songs appear after a library scan).
// ============================================================
#include "receiver.h"
#include "config.h"
#include "hal.h"
#include "settings.h"
#include <esp_heap_caps.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SD.h>

namespace Receiver {

// ============================ credentials ============================
struct RxCreds {
  uint32_t magic;
  char ssid[33];
  char pass[65];
  char tok[48];
};
static const uint32_t RX_MAGIC = 0xC0FFEE01;
static RxCreds creds;
static bool credsValid = false;
static volatile bool credsDirty = false;

static void credsLoad() {
  Preferences p;
  p.begin("rx", true);
  bool ok = p.getBytes("cfg", &creds, sizeof(creds)) == sizeof(creds) &&
            creds.magic == RX_MAGIC && creds.ssid[0] && creds.tok[0];
  p.end();
  credsValid = ok;
}
static void credsStore() {
  Preferences p;
  p.begin("rx", false);
  p.putBytes("cfg", &creds, sizeof(creds));
  p.end();
  credsValid = true;
  credsDirty = true;                       // main loop starts STA connect
}

// NVS on/off flag kept separate from the Settings blob so the player's
// saved config never rewrites it.
static void flagWrite(uint8_t v) {
  Preferences p;
  p.begin("mypod", false);
  p.putUChar("wifi", v);
  p.end();
}
static void exitToPlayer() {
  flagWrite(0);
  delay(120);
  ESP.restart();
}

bool enabled() {
  Preferences p;
  p.begin("mypod", true);
  uint8_t v = p.getUChar("wifi", 0);
  p.end();
  return v != 0;
}

void setEnabled(bool on) { flagWrite(on ? 1 : 0); }

// ============================ live state =============================
// written by main loop / rx task, drawn on screen and /status page
static volatile bool     staOk = false;
static char              staIp[16] = "";
static volatile int      rxCount = 0;         // mp3 saved this session
static volatile int      tgState = 0;         // 0 idle 1 polling 2 saving 3 autherr
static char              txtErr[64] = "";     // last human-readable problem
static char              apName[22] = "MyPod";
static bool              sdOk = false;
static volatile long long rxOffset = 0;      // Telegram update cursor

// tiny JSON helpers (Telegram responses, no full parser needed)
static int jsonStrVal(const String& body, const char* key, char* out, size_t cap, int from = 0) {
  String pat = String("\"") + key + "\":\"";
  int p = body.indexOf(pat, from);
  if (p < 0) return -1;
  p += pat.length();
  size_t o = 0;
  while (o + 1 < cap && p < (int)body.length()) {
    char c = body[p];
    if (c == '"') break;
    if (c == '\\' && p + 1 < (int)body.length()) {  // tiny unescape
      char n = body[p + 1];
      if (n == 'u' && p + 5 < (int)body.length()) {   // \uXXXX -> UTF-8
        char hex[5] = { body[p + 2], body[p + 3], body[p + 4], body[p + 5], 0 };
        unsigned cp = (unsigned)strtoul(hex, nullptr, 16);
        p += 6;
        if (cp >= 0xD800 && cp <= 0xDFFF) cp = '?';    // surrogate half: not drawable anyway
        if (cp < 0x80) {
          if (o + 1 < cap) out[o++] = (char)cp;
        } else if (cp < 0x800) {
          if (o + 2 < cap) { out[o++] = (char)(0xC0 | (cp >> 6)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
        } else {
          if (o + 3 < cap) { out[o++] = (char)(0xE0 | (cp >> 12)); out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
        }
        continue;
      }
      if (n == '"' || n == '\\' || n == '/') c = n;
      else if (n == 'n' || n == 't' || n == 'r') c = ' ';
      p++;
    }
    out[o++] = c;
    p++;
  }
  out[o] = '\0';
  return p;
}

static long long jsonNumVal(const String& body, const char* key, int from = 0) {
  String pat = String("\"") + key + "\":";
  int p = body.indexOf(pat, from);
  if (p < 0) return -1;
  p += pat.length();
  while (p < (int)body.length() && body[p] == ' ') p++;
  int sign = 1;
  if (body[p] == '-') { sign = -1; p++; }
  long long v = 0;
  while (p < (int)body.length() && isdigit((unsigned char)body[p])) {
    v = v * 10 + (body[p] - '0');
    p++;
  }
  return sign * v;
}

// Copies the JSON object that follows `"key":{` into `out`, with every NESTED
// {...} object dropped. Telegram's "document" object lists its "thumbnail"
// (a tiny cover JPEG with its OWN file_id) BEFORE the real file_id, so a plain
// "first file_id in the message" search downloads the thumbnail instead of the
// song. Flattening leaves only the object's own top-level fields.
static bool flatObject(const String& body, const char* key, String& out) {
  String pat = String("\"") + key + "\":{";
  int p = body.indexOf(pat);
  if (p < 0) return false;
  p += pat.length();
  int n = body.length();
  int depth = 1;
  bool inStr = false;
  out = "";
  for (; p < n && depth > 0; p++) {
    char c = body[p];
    if (inStr) {
      if (depth == 1) out += c;
      if (c == '\\' && p + 1 < n) { p++; if (depth == 1) out += body[p]; }
      else if (c == '"') inStr = false;
      continue;
    }
    if (c == '"') { inStr = true; if (depth == 1) out += c; }
    else if (c == '{') depth++;
    else if (c == '}') depth--;
    else if (depth == 1) out += c;
  }
  return true;
}

// "<base>.mp3" -> first free "<dir>/<base>.mp3", "<dir>/<base>_2.mp3", ...
// (the old code appended _2 AFTER .mp3, so duplicates were not seen as songs)
static void uniquePath(const char* dir, const char* sane, char* out, size_t cap) {
  char base[96];
  strlcpy(base, sane, sizeof(base));
  size_t bl = strnlen(base, sizeof(base));
  if (bl > 4 && strcasecmp(base + bl - 4, ".mp3") == 0) base[bl - 4] = '\0';
  snprintf(out, cap, "%s/%s.mp3", dir, base);
  for (int dup = 2; SD.exists(out) && dup < 99; dup++)
    snprintf(out, cap, "%s/%s_%d.mp3", dir, base, dup);
}

static void sanitizeName(const char* in, char* out, size_t cap) {
  size_t o = 0;
  for (const char* s = in; *s && o + 1 < cap - 5; s++) {
    unsigned char c = (unsigned char)*s;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == ' ' || c == '-' || c == '_' || c == '(' || c == ')' || c == '.') {
      if (c == '/' || c == '\\') c = '_';
      out[o++] = (char)c;
    } else if (c >= 0x80 && c < 0xC0) {
      // UTF-8 continuation byte: the lead byte already produced one '_'
    } else {
      out[o++] = '_';
    }
  }
  out[o] = '\0';
  // guarantee a lowercase .mp3 ending
  const char* dot = strrchr(out, '.');
  if (!dot || strcasecmp(dot, ".mp3") != 0) {
    out[o++] = '.'; out[o++] = 'm'; out[o++] = 'p'; out[o++] = '3'; out[o] = '\0';
  }
}

// percent-encode for SendMessage text (filenames may contain odd chars)
static void pctEncode(const char* in, String& out, size_t maxIn) {
  char buf[8];
  for (size_t i = 0; i < strnlen(in, maxIn); i++) {
    unsigned char c = (unsigned char)in[i];
    if (isdigit((unsigned char)c) || isalpha((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    }
  }
}

// trim spaces/tabs/newlines from user-typed text (portal forms add some)
static void trimTo(const char* in, char* out, size_t cap) {
  const char* s = in;
  const char* e = in + strnlen(in, cap * 2);
  while (s < e && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) s++;
  while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) e--;
  size_t n = min((size_t)(e - s), cap - 1);
  memcpy(out, s, n);
  out[n] = '\0';
}

// case-insensitive helpers (strcasestr is not in this core's newlib)
static bool ciEndsWith(const char* s, const char* suf) {
  size_t ls = strnlen(s, 100), lf = strnlen(suf, 20);
  return ls >= lf && strcasecmp(s + ls - lf, suf) == 0;
}
static bool ciContains(const char* hay, const char* nd) {
  size_t lh = strnlen(hay, 60), ln = strnlen(nd, 20);
  if (ln == 0 || lh < ln) return false;
  for (size_t i = 0; i + ln <= lh; i++) {
    if (strncasecmp(hay + i, nd, ln) == 0) return true;
  }
  return false;
}

static bool isAudioDoc(const char* mime, const char* name) {
  if (name && ciEndsWith(name, ".mp3")) return true;
  if (mime && ciContains(mime, "audio/")) return true;
  return false;
}

// ============================ telegram ==============================

// Telegram API base — switchable so a plain-HTTP relay on the laptop can
// stand in for TLS when the pod's heap cannot fit an mbedTLS session:
//   apibase http://192.168.x.x:8080/tg    (laptop proxy forwards to telegram)
static char apiBase[64] = "https://api.telegram.org";

// one GET against apiBase; body returned; returns HTTP code.
// picks TLS (WiFiClientSecure, insecure) or plain WiFiClient by URL prefix
static int apiGet(const char* url, String& body) {
  bool tls = strncasecmp(url, "https", 5) == 0;
  HTTPClient http;
  http.setTimeout(15000);
  http.setConnectTimeout(15000);
  WiFiClient plainCls;
  WiFiClientSecure tlCls;
  if (tls) tlCls.setInsecure();
  bool ok = tls ? http.begin(tlCls, url) : http.begin(plainCls, url);
  if (!ok) return -1;
  int code = http.GET();
  if (code > 0) body = http.getString();
  http.end();
  return code;
}

// 1 = saved, 0 = transient failure (retry), -1 = permanent failure (skip it)
static int saveMessage(long long chatId, long long updateId, const char* tok,
                       const char* fileId, const char* fileName);

static void sendChat(long long chatId, const char* tok, const char* text) {
  if (chatId <= 0) return;
  String enc;
  pctEncode(text, enc, 140);
  String aurl = apiBase;
  aurl += "/bot";
  aurl += tok;
  aurl += "/sendMessage?chat_id=";
  aurl += String((long long)chatId);
  aurl += "&text=";
  aurl += enc;
  String junk;
  apiGet(aurl.c_str(), junk);
}

// Same bus/speed as the player (library.cpp). Plain SD.begin(SD_CS) runs the
// card at 4 MHz, which made a multi-MB song take minutes to write.
static SPIClass rxSdSpi(VSPI);
static bool sdBegin() {
  rxSdSpi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  return SD.begin(SD_CS, rxSdSpi, 20000000);
}

static void rxTask(void*) {  if (!sdBegin()) {
    sdOk = false;
    snprintf(txtErr, sizeof(txtErr), "SD card: not found");
  } else {
    sdOk = true;
    SD.mkdir("/music");
    SD.mkdir("/music/telegram");
  }

  char fid[128] = "", fname[96] = "", mime[40] = "";
  int backoff = 0;

  for (;;) {
    bool credsNow;
    RxCreds c;
    if (credsDirty) {          // server /save handler published new creds
      credsLoad();
      credsDirty = false;
    }
    credsNow = credsValid;
    memcpy(&c, (const void*)&creds, sizeof(c));

    if (!sdOk) {                      // user may have inserted the card meanwhile
      if (sdBegin()) {
        sdOk = true;
        SD.mkdir("/music");
        SD.mkdir("/music/telegram");
        txtErr[0] = '\0';
      }
    }

    if (!credsNow) { snprintf(txtErr, sizeof(txtErr), "no credentials yet"); vTaskDelay(pdMS_TO_TICKS(1500)); continue; }
    if (WiFi.status() != WL_CONNECTED) {
      snprintf(txtErr, sizeof(txtErr), "home WiFi not connected");
      vTaskDelay(pdMS_TO_TICKS(1500));
      continue;
    }

    char url[250];
    snprintf(url, sizeof(url), "%s/bot%s/getUpdates?limit=1&offset=%lld&timeout=0",
             apiBase, c.tok, (long long)rxOffset);
    tgState = 1;
    String body;
    int code = apiGet(url, body);
    if (code == 401) {
      tgState = 3;
      snprintf(txtErr, sizeof(txtErr), "bot refused token (check it)");
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }
    if (code != 200) {
      if (millis() < 120000)                     // early diagnostics only
        Serial.printf("[RX] poll code=%d heap=%u bigfree=%u\n", code,
                      (unsigned)ESP.getFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
      snprintf(txtErr, sizeof(txtErr), "telegram poll failed (%d)", code);
      vTaskDelay(pdMS_TO_TICKS(3000));
      continue;
    }
    tgState = 0;
    txtErr[0] = '\0';

    long long id = jsonNumVal(body, "update_id");
    if (id < 0) {                    // inbox empty
      vTaskDelay(pdMS_TO_TICKS(2500));
      continue;
    }
    // first "id" in the message = sender's id; in a private bot DM that IS the
    // chat id, so the acknowledgement can be addressed without extra parsing
    long long chatId = jsonNumVal(body, "id");

    // inspect the update for an audio / document attachment. Only the
    // attachment object's OWN fields are read (see flatObject): its nested
    // thumbnail has a different file_id that must never be downloaded.
    bool handled = false;
    String obj;
    bool isAudioMsg = flatObject(body, "audio", obj);
    if (!isAudioMsg && !flatObject(body, "document", obj)) {
      handled = true;                    // text/photo/voice/etc: not a song -> advance
    } else {
      char title[64] = "", perf[48] = "";
      fid[0] = fname[0] = mime[0] = '\0';
      jsonStrVal(obj, "file_id", fid, sizeof(fid));
      jsonStrVal(obj, "file_name", fname, sizeof(fname));
      jsonStrVal(obj, "mime_type", mime, sizeof(mime));
      jsonStrVal(obj, "title", title, sizeof(title));
      jsonStrVal(obj, "performer", perf, sizeof(perf));
      // music-player sends may carry no file_name: build one from the tags
      // Telegram gives us, so the song is at least named "Artist - Title"
      if (!fname[0]) {
        if (perf[0] && title[0]) snprintf(fname, sizeof(fname), "%s - %s", perf, title);
        else if (title[0])       strlcpy(fname, title, sizeof(fname));
        else                     snprintf(fname, sizeof(fname), "audio_%lld", (long long)id + 1);
      }
      if (!fid[0] || !(isAudioMsg || isAudioDoc(mime, fname))) {
        handled = true;                  // not a song: skip it, advance offset
      } else {
        int r = saveMessage(chatId, id, c.tok, fid, fname);
        if (r == 1) {
          rxCount++;
          Cfg::setLibDirty(true);        // the player rebuilds /music.idx on next boot
          handled = true;
        } else if (r < 0) {
          handled = true;                // permanent failure, user was told in chat
        }
      }
    }

    if (handled) {
      rxOffset = id + 1;
      backoff = 0;
    } else {
      // one bad file must not wedge the whole queue: retry briefly, then skip
      backoff++;
      vTaskDelay(pdMS_TO_TICKS(backoff > 3 ? 8000 : 3000));
      if (backoff > 2) {                   // a retry re-downloads the whole song: try 3x, then move on
        Serial.printf("[RX] save failed for %s - skipping\n", fname);
        rxOffset = id + 1;
        backoff = 0;
      } else {
        continue;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1200));
  }
}

static int saveMessage(long long chatId, long long updateId, const char* tok,
                       const char* fileId, const char* fileName) {
  (void)updateId;

  // 1. resolve file path (Telegram file ids are URL-safe characters)
  char url[400];
  snprintf(url, sizeof(url), "%s/bot%s/getFile?file_id=%s", apiBase, tok, fileId);
  String body;
  int code = apiGet(url, body);
  if (code == 400) {
    // Bot API refuses getFile for files over 20 MB (or an expired id): retrying
    // can never work, so say so in the chat instead of failing silently.
    snprintf(txtErr, sizeof(txtErr), "getFile refused (400)");
    sendChat(chatId, tok, "MyPod: Telegram bots can only download files up to 20 MB. "
                          "Send a smaller MP3 (or copy it to the SD card).");
    return -1;
  }
  if (code != 200) {
    snprintf(txtErr, sizeof(txtErr), "getFile failed (%d)", code);
    return 0;
  }
  char fpath[200];
  if (jsonStrVal(body, "file_path", fpath, sizeof(fpath)) < 0) {
    snprintf(txtErr, sizeof(txtErr), "file_path missing");
    return 0;
  }
  long long fsz = jsonNumVal(body, "file_size");
  if (fsz > (long long)100 * 1024 * 1024) {
    snprintf(txtErr, sizeof(txtErr), "file too big to save");
    return -1;
  }

  // 2. build a safe destination name
  char sane[96];
  sanitizeName(fileName, sane, sizeof(sane));
  char finalPath[PATH_MAX_LEN];
  uniquePath("/music/telegram", sane, finalPath, sizeof(finalPath));

  // 3. stream the file down (HTTPClient streams, no full-body buffer)
  tgState = 2;
  snprintf(txtErr, sizeof(txtErr), "saving %s...", sane);
  bool dls = strncasecmp(apiBase, "https", 5) == 0;   // same scheme as the API
  snprintf(url, sizeof(url), "%s/file/bot%s/%s", apiBase, tok, fpath);
  HTTPClient http;
  http.setTimeout(30000);                   // read timeout (relay fetches before replying)
  http.setConnectTimeout(8000);
  WiFiClient wcDl;                          // must outlive the transfer
  WiFiClientSecure tlDl;
  bool opened;
  if (dls) {
    tlDl.setInsecure();
    opened = http.begin(tlDl, url);
  } else {
    opened = http.begin(wcDl, url);
  }
  if (!opened) {
    tgState = 1;
    snprintf(txtErr, sizeof(txtErr), "file download open failed");
    return 0;
  }
  int code2 = http.GET();
  if (code2 != 200) {
    http.end();
    tgState = 1;
    snprintf(txtErr, sizeof(txtErr), "download failed (%d)", code2);
    return 0;
  }
  File f = SD.open(finalPath, FILE_WRITE);
  if (!f) {
    http.end();
    tgState = 1;
    snprintf(txtErr, sizeof(txtErr), "SD write failed");
    return 0;
  }
  int total = http.getSize();               // -1 when chunked
  if (total <= 0 && fsz > 0) total = (int)fsz;
  WiFi.setSleep(false);                     // modem sleep throttles throughput badly

  // Read from the socket into a large buffer and write the SD in big blocks
  // (HTTPClient::writeToStream writes ~1.4 KB at a time = slow on SPI SD).
  const size_t BUFSZ = 8192;
  uint8_t* buf = (uint8_t*)malloc(BUFSZ);
  size_t bsz = BUFSZ;
  if (!buf) { bsz = 1024; buf = (uint8_t*)malloc(bsz); }
  if (!buf) {
    f.close(); http.end(); SD.remove(finalPath);
    tgState = 1;
    snprintf(txtErr, sizeof(txtErr), "out of memory");
    return 0;
  }
  WiFiClient* stream = http.getStreamPtr();
  int wr = 0;                               // bytes written to SD
  bool sdFail = false;
  uint32_t lastData = millis(), lastShow = 0, t0 = millis();
  while ((http.connected() || stream->available()) && (total <= 0 || wr < total)) {
    int avail = stream->available();
    if (avail > 0) {
      int n = stream->readBytes(buf, (size_t)avail > bsz ? bsz : (size_t)avail);
      if (n <= 0) continue;
      if ((int)f.write(buf, n) != n) { sdFail = true; break; }
      wr += n;
      lastData = millis();
      if (millis() - lastShow > 700) {      // percent first: the TFT shows ~24 chars
        lastShow = millis();
        if (total > 0) snprintf(txtErr, sizeof(txtErr), "saving %d%% %s", (int)((long long)wr * 100 / total), sane);
        else           snprintf(txtErr, sizeof(txtErr), "saving %dKB %s", wr / 1024, sane);
      }
    } else {
      if (millis() - lastData > 15000) break;   // stalled: give up, retry
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }
  free(buf);
  f.close();
  http.end();
  tgState = 1;
  Serial.printf("[RX] download %d/%d bytes in %lu ms%s\n", wr, total,
                (unsigned long)(millis() - t0), sdFail ? " (SD write error)" : "");
  if (sdFail) wr = -1;

  // 4. verify what actually landed on the card: right size, and really an
  // MP3 stream (a cover thumbnail / error page must never pass as a song)
  bool bad = (wr <= 0) || (total > 0 && wr < total) || (fsz > 0 && wr < fsz);
  uint8_t mg[4] = {0, 0, 0, 0};
  if (!bad) {
    File chk = SD.open(finalPath, FILE_READ);
    size_t sz = chk ? chk.size() : 0;
    if (chk) { chk.read(mg, 4); chk.close(); }
    if (sz != (size_t)wr || sz < 4) bad = true;
    if (mg[0] == 0xFF && mg[1] == 0xD8) bad = true;                  // JPEG
    if (mg[0] == 0x89 && mg[1] == 'P' && mg[2] == 'N') bad = true;   // PNG
    if (mg[0] == '<' || mg[0] == '{') bad = true;                    // HTML / JSON error body
  }
  if (bad) {
    SD.remove(finalPath);
    snprintf(txtErr, sizeof(txtErr), "download incomplete");
    Serial.printf("[RX] bad download for %s (wr=%d total=%d expect=%lld)\n",
                  sane, wr, total, fsz);
    return 0;
  }
  snprintf(txtErr, sizeof(txtErr), "saved %s", sane);
  Serial.printf("[RX] saved %s (%d bytes)\n", finalPath, wr);

  // 5. acknowledge in the chat so the user sees it landed (needs chat_id!)
  String text = "Saved to MyPod: ";
  text += sane;
  sendChat(chatId, tok, text.c_str());
  return 1;
}

// ============================ web portal ============================
static WebServer server(80);

static void pageHeader(String& s, const char* title) {
  s = "<!doctype html><html><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>"; s += title; s += "</title>"
      "<style>body{font-family:-apple-system,sans-serif;background:#111;color:#eee;"
      "max-width:320px;margin:0 auto;padding:14px;}h2{font-size:18px;margin:8px 0;}"
      "input{width:100%;box-sizing:border-box;padding:9px;margin:5px 0;border-radius:8px;"
      "border:1px solid #444;background:#1c1c1e;color:#eee;font-size:15px;}"
      "button,a.btn{width:100%;padding:10px;margin:6px 0;border:0;border-radius:8px;"
      "background:#f52e50;color:#fff;font-size:15px;text-decoration:none;display:block;text-align:center;}"
      "a.link{color:#8ab4ff;}p{font-size:13px;margin:6px 0;white-space:pre-wrap;}</style></head><body><h2>";
  s += title; s += "</h2>";
}

static void hRoot() {
  String s;
  pageHeader(s, "MyPod setup");
  s += "<p>Enter the WiFi of your home and the Telegram bot token that "
       "will send MP3s to this pod.</p>";
  s += "<form method='GET' action='/save'>";
  s += "<input name='ssid' placeholder='Home WiFi name' value=''>"
       "<input name='pass' type='password' placeholder='Home WiFi password'>"
       "<input name='tok' placeholder='Telegram bot token'>"
       "<button type='submit'>Save &amp; connect</button></form>";
  s += "<a class='btn' style='background:#333' href='/status'>Status</a>";
  s += "<a class='btn' style='background:#333' href='/exit'>Turn off receiver &amp; return to player</a>";
  s += "<p id='st'></p>";
  server.send(200, "text/html", s);
}

static void hSave() {
  if (server.hasArg("ssid") && server.hasArg("tok") && server.arg("ssid").length() > 0 &&
      server.arg("tok").length() > 0) {
    trimTo(server.arg("ssid").c_str(), creds.ssid, sizeof(creds.ssid));
    if (server.hasArg("pass")) trimTo(server.arg("pass").c_str(), creds.pass, sizeof(creds.pass));
    else creds.pass[0] = '\0';
    char tokIn[64];
    trimTo(server.arg("tok").c_str(), tokIn, sizeof(tokIn));
    strlcpy(creds.tok, tokIn, sizeof(creds.tok));
    creds.magic = RX_MAGIC;
    credsStore();
  }
  server.sendHeader("Location", "/status");
  server.send(302, "text/plain", "");
}

static void hStatus() {
  String s;
  pageHeader(s, "Receiver status");
  s += "<script>setInterval(async()=>{let t=await(await fetch('/status.json')).text();"
       "document.getElementById('st').textContent=t;},2000);</script>";
  server.send(200, "text/html", s);
}

static void encHtml(String& s, const char* txt) {
  for (const char* q = txt; *q; q++) {
    if (*q == '<') s += "&lt;";
    else if (*q == '>') s += "&gt;";
    else if (*q == '&') s += "&amp;";
    else s += *q;
  }
}

static void hStatusJson() {
  String s;
  char line[160];
  snprintf(line, sizeof(line), "Hotspot: %s / 12345678\n", apName);  s += line;
  s += WiFi.status() == WL_CONNECTED ? "Home WiFi: connected @ " : "Home WiFi: not connected";
  if (WiFi.status() == WL_CONNECTED) {
    encHtml(s, WiFi.localIP().toString().c_str());
    snprintf(line, sizeof(line), "  (%d dBm)\n", WiFi.RSSI());
  } else {
    s += "\n";
  }
  s += ("Token: ");
  s += (tgState == 3 ? "REFUSED - check it" : (credsValid ? "accepted" : "not yet"));
  s += "\n";
  if (tgState == 2) s += "Saving to SD...\n";
  else if (rxCount) s += "MP3 received: ";
  else s += "MP3 received: ";
  s += String(rxCount) + "\n";
  if (txtErr[0]) { encHtml(s, txtErr); s += "\n"; }
  if (!sdOk) s += "SD card: MISSING / bad - no files will save\n";
  s += "Browse '/' to edit settings\n";
  server.send(200, "text/plain", s);
}

static void hExit() {
  flagWrite(0);
  server.send(200, "text/html",
              "<!doctype html><body style='background:#111;color:#eee;"
              "font-family:sans-serif'><h2>Receiver off.<br>Pod restarting into the player...</h2></body>");
  delay(600);
  ESP.restart();
}
// ============================ screen ================================

static void drawStatic() {
  Hal::tft.fillScreen(TFT_BLACK);
  Hal::tft.setTextColor(TFT_CYAN, TFT_BLACK);
  Hal::tft.setTextFont(2);
  Hal::tft.setTextDatum(TL_DATUM);
  Hal::tft.drawString("WIFI RECEIVER", 8, 8);
  Hal::tft.setTextColor(TFT_WHITE, TFT_BLACK);
  Hal::tft.drawString("1) Join this hotspot:", 8, 40);
  Hal::tft.setTextColor(TFT_RED, TFT_BLACK);
  Hal::tft.drawString(apName, 24, 62);
  Hal::tft.drawString("pass: 12345678", 24, 80);
  Hal::tft.setTextColor(TFT_WHITE, TFT_BLACK);
  Hal::tft.drawString("2) open the site:", 8, 112);
  Hal::tft.setTextColor(TFT_RED, TFT_BLACK);
  Hal::tft.drawString("http://192.168.4.1", 8, 132);
  Hal::tft.setTextColor(TFT_WHITE, TFT_BLACK);
  Hal::tft.drawString("Hold BOOT 3s = player", 8, 296);
}

// wipes /music recursively + /music.idx — fresh start for the library
static bool rmTree(const char* path) {
  bool ok = true;
  File d = SD.open(path);
  if (!d) return true;                       // already gone
  if (!d.isDirectory()) { d.close(); return SD.remove(path); }
  for (;;) {
    File f = d.openNextFile();
    if (!f) break;
    char child[PATH_MAX_LEN];
    snprintf(child, sizeof(child), "%s/%s", path, f.name());
    bool isDir = f.isDirectory();
    f.close();
    if (isDir) ok &= rmTree(child);
    else ok &= SD.remove(child);
  }
  d.close();
  return ok && SD.rmdir(path);
}

// --- plain-HTTP pull (no TLS at all): serial cmd
//   pull http://laptop:port/some%20name.mp3 [some%20name.mp3]
// the laptop provides the file when Telegram TLS cannot fit in RAM
static void urlDecode(const char* src, char* dst, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; src[i] && o + 1 < cap; i++) {
    if (src[i] == '%' && src[i + 1] && src[i + 2]) {
      char h[3] = { src[i + 1], src[i + 2], 0 };
      dst[o++] = (char)strtol(h, nullptr, 16);
      i += 2;
    } else {
      dst[o++] = src[i];
    }
  }
  dst[o] = 0;
}

static void pullUrl(const char* args) {
  char tmp[300];
  strlcpy(tmp, args, sizeof(tmp));
  char* sp = strchr(tmp, ' ');
  if (sp) *sp = '\0';                        // url part
  const char* arg2 = sp ? sp + 1 : nullptr;

  const char* url = tmp;                     // keep %20 encoded on the wire
  if (strncasecmp(url, "http://", 7) != 0) {
    Serial.println("[RX] usage: pull http://laptop:port/file%20name.mp3 [file%20name.mp3]");
    return;
  }
  char fname[128];                           // decoded destination name
  const char* b = strrchr(url, '/');         // destination: arg2 or URL basename
  urlDecode(arg2 && *arg2 ? arg2 : (b ? b + 1 : ""), fname, sizeof(fname));
  if (!fname[0]) { Serial.println("[RX] pull: no file name"); return; }

  char sane[96];
  sanitizeName(fname, sane, sizeof(sane));
  char finalPath[PATH_MAX_LEN];
  uniquePath("/music", sane, finalPath, sizeof(finalPath));

  WiFiClient wc;                             // plain HTTP, no TLS buffers
  HTTPClient http;
  http.setTimeout(8000);
  http.setConnectTimeout(8000);
  if (!http.begin(wc, url)) { Serial.println("[RX] pull: connect failed"); return; }
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[RX] pull failed (%d)\n", code);
    http.end();
    return;
  }
  File f = SD.open(finalPath, FILE_WRITE);
  if (!f) { Serial.println("[RX] pull: SD open failed"); http.end(); return; }
  int wr = http.writeToStream(&f);
  f.close();
  http.end();
  if (wr <= 0) { Serial.println("[RX] pull: transfer aborted"); return; }
  Cfg::setLibDirty(true);                    // player rebuilds /music.idx next boot
  Serial.printf("[RX] pulled %s (%d bytes)\n", finalPath, wr);
}

// ============================ API ===================================

void run() {
  Serial.begin(115200);
  delay(120);
  Serial.println();
  Serial.println("[RX] WiFi receiver mode");

  credsLoad();
  {                            // remember the bridge/base URL across reboots
    Preferences p;
    p.begin("rx", true);
    p.getString("abase", apiBase, sizeof(apiBase));   // keeps https default if absent
    p.end();
  }
  WiFi.mode(WIFI_AP_STA);

  String mac = WiFi.macAddress();
  mac.replace(":", "");
  snprintf(apName, sizeof(apName), "MyPod-%s", mac.substring(8, 12).c_str());
  WiFi.softAP(apName, "12345678");
  delay(150);

  server.on("/", hRoot);
  server.on("/save", hSave);
  server.on("/status", hStatus);
  server.on("/status.json", hStatusJson);
  server.on("/exit", hExit);
  server.begin();

  xTaskCreatePinnedToCore(rxTask, "tgrx", 12288, nullptr, 3, nullptr, 0);

  drawStatic();

  uint32_t lastUi = 0, lastHomeTry = 0;
  int bootHold = 0;
  bool homeAttempted = false;
  bool staMode = false;                      // AP down once home WiFi confirms

  for (;;) {
    server.handleClient();
    delay(4);

    // BOOT button: 3 s hold = exit
    if (digitalRead(PIN_BOOT) == LOW) {
      bootHold += 8;
      if (bootHold > 3000) exitToPlayer();
    } else {
      bootHold = 0;
    }

    // serial console for the laptop (no phone needed)
    static char lbuf[300];
    static size_t ln = 0;
    while (Serial.available()) {
      char ch = Serial.read();
      if (ch == '\r') continue;
      if (ch == '\n') {
        lbuf[ln] = '\0';
        if (ln) {
          if (!strcasecmp(lbuf, "exit")) exitToPlayer();
          else if (!strcasecmp(lbuf, "info")) {
            Serial.printf("[RX] ap=%s ip=%s home=%s(%s=%d) tok=%s sd=%d saved=%d heap=%u err=%s\n",
                          apName, WiFi.softAPIP().toString().c_str(),
                          (WiFi.status() == WL_CONNECTED ? "up" : "down"),
                          creds.ssid, WiFi.status(), creds.tok, (int)sdOk, rxCount,
                          (unsigned)ESP.getFreeHeap(), txtErr);
          } else if (!strncasecmp(lbuf, "cred ", 5)) {
            // cred <ssid> <password> <token>
            char a[3][70];
            if (sscanf(lbuf + 5, "%69s %69s %69s", a[0], a[1], a[2]) == 3) {
              strlcpy(creds.ssid, a[0], sizeof(creds.ssid));
              strlcpy(creds.pass, a[1], sizeof(creds.pass));
              strlcpy(creds.tok, a[2], sizeof(creds.tok));
              creds.magic = RX_MAGIC;
              credsStore();
              Serial.println("[RX] creds stored");
            } else Serial.println("[RX] usage: cred ssid pass tok");
          } else if (!strncasecmp(lbuf, "apibase ", 8)) {
            strlcpy(apiBase, lbuf + 8, sizeof(apiBase));
            { Preferences p; p.begin("rx", false); p.putString("abase", apiBase); p.end(); }
            Serial.printf("[RX] api base: %s (saved)\n", apiBase);
          } else if (!strncasecmp(lbuf, "rm ", 3)) {
            // rm /music/<name.mp3> - delete a file from the card
            char pth[PATH_MAX_LEN];
            urlDecode(lbuf + 3, pth, sizeof(pth));
            if (strncasecmp(pth, "/music/", 7) != 0 || strstr(pth, "..")) {
              Serial.println("[RX] rm: only /music/... paths");
            } else if (!SD.exists(pth)) {
              Serial.printf("[RX] rm: no such file %s\n", pth);
            } else {
              SD.remove(pth);
              Cfg::setLibDirty(true);
              Serial.printf("[RX] rm: deleted %s\n", pth);
            }
          } else if (!strcasecmp(lbuf, "rmall")) {
            // delete every song + the index (fresh library start)
            if (rmTree("/music")) Serial.println("[RX] rmall: /music cleared");
            else Serial.println("[RX] rmall: some entries refused, retry");
            SD.mkdir("/music");                  // pulls expect the folder to exist
            SD.remove("/music.idx");
            Cfg::setLibDirty(true);
          } else if (!strncasecmp(lbuf, "pull ", 5)) {
            pullUrl(lbuf + 5);              // file over plain HTTP from laptop
          } else Serial.println("[RX] ? (exit/info/apibase url/pull url/rmall/cred ssid pass tok)");
        }
        ln = 0;
        continue;
      }
      if (ln < sizeof(lbuf) - 1) lbuf[ln++] = ch;
    }

    // drive the home-network connect attempt (STA)
    if (credsValid && WiFi.status() != WL_CONNECTED && millis() - lastHomeTry > 12000) {
      lastHomeTry = millis();
      Serial.printf("[RX] WiFi.begin(%s)\n", creds.ssid);
      WiFi.begin(creds.ssid, creds.pass);
      homeAttempted = true;
    }
    if (!credsValid) homeAttempted = false;

    // once home WiFi is up, retire the hotspot and soften the radio RAM
    // pressure — mbedTLS needs ~45 KB heap and the AP mode held part of it
    if (!staMode && WiFi.status() == WL_CONNECTED) {
      staMode = true;
      WiFi.setSleep(true);
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_MODE_STA);
      Serial.printf("[RX] home up %s heap=%u\n",
                    WiFi.localIP().toString().c_str(), (unsigned)ESP.getFreeHeap());
    }

    // status lines under the fixed instructions (kept short for the TFT)
    if (millis() - lastUi > 1000) {
      lastUi = millis();
      Hal::tft.setTextFont(2);
      Hal::tft.setTextColor(TFT_WHITE, TFT_BLACK);
      Hal::tft.fillRect(0, 172, 240, 116, TFT_BLACK);

      char buf[30];
      if (WiFi.status() == WL_CONNECTED) {
        strlcpy(buf, "home: up", sizeof(buf));
      } else if (credsValid) {
        strlcpy(buf, "home: connecting...", sizeof(buf));
      } else {
        strlcpy(buf, "home: not configured", sizeof(buf));
      }
      Hal::tft.drawString(buf, 8, 176);

      if (WiFi.status() == WL_CONNECTED) {
        snprintf(buf, sizeof(buf), "ip: %s", WiFi.localIP().toString().c_str());
        Hal::tft.drawString(buf, 8, 196);
      }

      if (tgState == 3) strlcpy(buf, "token: REFUSED!", sizeof(buf));
      else if (!credsValid) strlcpy(buf, "token: not set", sizeof(buf));
      else if (tgState == 1) strlcpy(buf, "telegram: polling", sizeof(buf));
      else if (tgState == 2) strlcpy(buf, "telegram: saving...", sizeof(buf));
      else strlcpy(buf, "telegram: waiting", sizeof(buf));
      Hal::tft.drawString(buf, 8, 220);

      snprintf(buf, sizeof(buf), "mp3 saved: %d", rxCount);
      Hal::tft.drawString(buf, 8, 244);

      if (!sdOk) {
        Hal::tft.setTextColor(TFT_RED, TFT_BLACK);
        Hal::tft.drawString("SD CARD MISSING!", 8, 268);
        Hal::tft.setTextColor(TFT_WHITE, TFT_BLACK);
      } else if (txtErr[0]) {
        Hal::tft.setTextColor(TFT_ORANGE, TFT_BLACK);
        char e[24];
        strlcpy(e, txtErr, sizeof(e));
        Hal::tft.drawString(e, 8, 268);
        Hal::tft.setTextColor(TFT_WHITE, TFT_BLACK);
      }
    }
  }
}

}  // namespace Receiver
