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
//       your bot; audio/mp3 documents are saved to /music/telegram.
//    3. Exit: /exit in the browser, or hold BOOT for 3 s. Normal
//       player resumes (received songs appear after a library scan).
// ============================================================
#include "receiver.h"
#include "config.h"
#include "hal.h"
#include "settings.h"
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
      if (n == '"' || n == '\\' || n == '/') c = n;
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

static void sanitizeName(const char* in, char* out, size_t cap) {
  size_t o = 0;
  for (const char* s = in; *s && o + 1 < cap - 5; s++) {
    unsigned char c = (unsigned char)*s;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == ' ' || c == '-' || c == '_' || c == '(' || c == ')' || c == '.') {
      if (c == '/' || c == '\\') c = '_';
      out[o++] = (char)c;
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

// one HTTPS request/response cycle; body returned; returns HTTP code
static int tgGet(WiFiClientSecure& tl, const char* url, String& body) {
  HTTPClient http;
  http.setTimeout(15000);
  http.setConnectTimeout(15000);
  if (!http.begin(tl, url)) return -1;
  int code = http.GET();
  if (code > 0) body = http.getString();
  http.end();
  return code;
}

static bool saveMessage(long long chatId, long long updateId, const char* tok,
                        const char* fileId, const char* fileName);

static void rxTask(void*) {  if (!SD.begin(SD_CS)) {
    sdOk = false;
    snprintf(txtErr, sizeof(txtErr), "SD card: not found");
  } else {
    sdOk = true;
    SD.mkdir("/music");
    SD.mkdir("/music/telegram");
  }

  WiFiClientSecure tl;
  tl.setInsecure();

  char fid[80] = "", fname[96] = "", mime[40] = "";
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
      if (SD.begin(SD_CS)) {
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
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/getUpdates?limit=1&offset=%lld&timeout=0",
             c.tok, (long long)rxOffset);
    tgState = 1;
    String body;
    int code = tgGet(tl, url, body);
    if (code == 401) {
      tgState = 3;
      snprintf(txtErr, sizeof(txtErr), "bot refused token (check it)");
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }
    if (code != 200) {
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

    // inspect the update for a document/audio attachment
    bool handled = false;
    int p = 0;
    while (!handled) {
      p = jsonStrVal(body, "file_id", fid, sizeof(fid), p);
      if (p < 0) break;
      if (jsonStrVal(body, "file_name", fname, sizeof(fname), p) < 0) fname[0] = '\0';
      if (jsonStrVal(body, "mime_type", mime, sizeof(mime), p) < 0) mime[0] = '\0';
      // audio-style sends (Telegram's music player) carry no file_name: fall
      // back to the track title, then a numbered name so songs never collide
      if (!fname[0] && jsonStrVal(body, "title", fname, sizeof(fname), p) < 0)
        fname[0] = '\0';
      if (!fname[0]) snprintf(fname, sizeof(fname), "audio_%lld", (long long)id + 1);
      if (isAudioDoc(mime, fname)) {
        handled = saveMessage(chatId, id, c.tok, fid, fname);
        if (handled) {
          rxCount++;
          Cfg::setLibDirty(true);          // the player rebuilds /music.idx on next boot
        }
      } else {
        handled = true;              // not a song: skip it, advance offset
      }
      p += 1;
    }

    if (handled) {
      rxOffset = id + 1;
      backoff = 0;
    } else {
      // download failed (usually no SD / card error): retry same update later
      backoff++;
      vTaskDelay(pdMS_TO_TICKS(backoff > 2 ? 10000 : 3000));
      continue;
    }
    vTaskDelay(pdMS_TO_TICKS(1200));
  }
}

static bool saveMessage(long long chatId, long long updateId, const char* tok,
                        const char* fileId, const char* fileName) {
  (void)updateId;
  WiFiClientSecure tl;
  tl.setInsecure();

  // 1. resolve file path (Telegram file ids are URL-safe characters)
  char url[400];
  snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/getFile?file_id=%s", tok, fileId);
  String body;
  int code = tgGet(tl, url, body);
  if (code != 200) {
    snprintf(txtErr, sizeof(txtErr), "getFile failed (%d)", code);
    return false;
  }
  char fpath[200];
  if (jsonStrVal(body, "file_path", fpath, sizeof(fpath)) < 0) {
    snprintf(txtErr, sizeof(txtErr), "file_path missing");
    return false;
  }
  long long fsz = jsonNumVal(body, "file_size");
  if (fsz > (long long)100 * 1024 * 1024) {
    snprintf(txtErr, sizeof(txtErr), "file too big to save");
    return false;
  }

  // 2. build a safe destination name
  char sane[96];
  sanitizeName(fileName, sane, sizeof(sane));
  char path[PATH_MAX_LEN];
  snprintf(path, sizeof(path), "/music/telegram/%s", sane);
  char finalPath[PATH_MAX_LEN];
  strlcpy(finalPath, path, sizeof(finalPath));
  int dup = 2;
  while (SD.exists(finalPath) && dup < 99) {
    snprintf(finalPath, sizeof(finalPath), "%s_%d", path, dup);
    dup++;
  }

  // 3. stream the file down (HTTPClient streams, no full-body buffer)
  tgState = 2;
  snprintf(txtErr, sizeof(txtErr), "saving %s...", sane);
  HTTPClient http;
  http.setTimeout(8000);                    // read timeout
  http.setConnectTimeout(8000);
  snprintf(url, sizeof(url), "https://api.telegram.org/file/bot%s/%s", tok, fpath);
  if (!http.begin(tl, url)) {
    tgState = 1;
    snprintf(txtErr, sizeof(txtErr), "file download open failed");
    return false;
  }
  int code2 = http.GET();
  if (code2 != 200) {
    http.end();
    tgState = 1;
    snprintf(txtErr, sizeof(txtErr), "download failed (%d)", code2);
    return false;
  }
  File f = SD.open(finalPath, FILE_WRITE);
  if (!f) {
    http.end();
    tgState = 1;
    snprintf(txtErr, sizeof(txtErr), "SD write failed");
    return false;
  }
  int total = http.getSize();               // -1 when chunked
  int wr = http.writeToStream(&f);
  f.close();
  http.end();
  tgState = 1;
  if (wr <= 0 || (total > 0 && wr < total)) {
    SD.remove(finalPath);
    snprintf(txtErr, sizeof(txtErr), "download incomplete");
    return false;
  }
  snprintf(txtErr, sizeof(txtErr), "saved %s", sane);
  Serial.printf("[RX] saved %s (%d bytes)\n", finalPath, wr);

  // 4. acknowledge in the chat so the user sees it landed (needs chat_id!)
  if (chatId > 0) {
    WiFiClientSecure tl2;
    tl2.setInsecure();
    String text = "Saved to MyPod: ";
    text += sane;
    String enc;
    pctEncode(text.c_str(), enc, 140);
    String aurl = "https://api.telegram.org/bot";
    aurl += tok;
    aurl += "/sendMessage?chat_id=";
    aurl += String((long long)chatId);
    aurl += "&text=";
    aurl += enc;
    String junk;
    tgGet(tl2, aurl.c_str(), junk);
  }
  return true;
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

// ============================ API ===================================

void run() {
  Serial.begin(115200);
  delay(120);
  Serial.println();
  Serial.println("[RX] WiFi receiver mode");

  credsLoad();
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

  xTaskCreatePinnedToCore(rxTask, "tgrx", 24576, nullptr, 3, nullptr, 0);

  drawStatic();

  uint32_t lastUi = 0, lastHomeTry = 0;
  int bootHold = 0;
  bool homeAttempted = false;

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
    static char lbuf[80];
    static size_t ln = 0;
    while (Serial.available()) {
      char ch = Serial.read();
      if (ch == '\r') continue;
      if (ch == '\n') {
        lbuf[ln] = '\0';
        if (ln) {
          if (!strcasecmp(lbuf, "exit")) exitToPlayer();
          else if (!strcasecmp(lbuf, "info")) {
            Serial.printf("[RX] ap=%s ip=%s home=%s(%s=%d) tok=%s sd=%d saved=%d err=%s\n",
                          apName, WiFi.softAPIP().toString().c_str(),
                          (WiFi.status() == WL_CONNECTED ? "up" : "down"),
                          creds.ssid, WiFi.status(), creds.tok, (int)sdOk, rxCount, txtErr);
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
          } else Serial.println("[RX] ? (exit/info/cred ssid pass tok)");
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
