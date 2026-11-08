#pragma once
// HTTP API + static UI. POST bodies are form-encoded (no JSON parser needed on the device).
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <memory>
#include "state.h"
#include "storage.h"
#include "sensors.h"
#include "auth.h"
#include "session.h"
#include "upload.h"
#include "risk.h"

using Req = AsyncWebServerRequest;

static String P(Req* r, const char* k) {
  return r->hasParam(k, true) ? r->getParam(k, true)->value() : String();
}
static bool hasP(Req* r, const char* k) { return r->hasParam(k, true); }

static void sendDoc(Req* r, JsonDocument& d, int code = 200) {
  AsyncResponseStream* o = r->beginResponseStream("application/json");
  o->setCode(code);
  o->addHeader("Cache-Control", "no-store");
  serializeJson(d, *o);
  r->send(o);
}
static void jerr(Req* r, int code, const char* m) {
  JsonDocument d;
  d["error"] = m;
  sendDoc(r, d, code);
}
static void jok(Req* r) {
  JsonDocument d;
  d["ok"] = true;
  sendDoc(r, d);
}
static bool needAdmin(Req* r) {
  touch();
  if (authCheck(r)) return true;
  jerr(r, 401, "login required");
  return false;
}
static void pn(Print* o, double v, uint8_t d) {
  if (isnan(v)) o->print("null"); else o->print(v, d);
}

static void handleRiskList(Req* r) {
  touch();
  if (!sdOk) { jerr(r, 503, "SD card not available"); return; }
  SdLock lock;
  if (!lock.ok) { jerr(r, 503, "SD busy"); return; }
  File f = SD.open(RISK_FILE, FILE_READ);
  AsyncResponseStream* response = r->beginResponseStream("application/json");
  response->addHeader("Cache-Control", "no-store");
  response->print("{\"events\":[");
  if (f) {
    uint32_t count = 0;
    char line[RISK_LINE_SIZE];
    while (f.available()) {
      int n = f.readBytesUntil('\n', line, sizeof(line) - 1);
      line[n] = 0;
      if (n) count++;
    }
    f.close();
    f = SD.open(RISK_FILE, FILE_READ);
    uint32_t skip = count > RISK_LIST_LIMIT ? count - RISK_LIST_LIMIT : 0;
    uint32_t index = 0;
    bool first = true;
    while (f && f.available()) {
      int n = f.readBytesUntil('\n', line, sizeof(line) - 1);
      line[n] = 0;
      if (index++ < skip || !n) continue;
      JsonDocument event;
      if (deserializeJson(event, line)) continue;
      if (!first) response->print(',');
      first = false;
      serializeJson(event, *response);
    }
    if (f) f.close();
  }
  response->print(authCheck(r) ? "],\"admin\":true}" : "],\"admin\":false}");
  r->send(response);
}

static void handleRiskLog(Req* r) {
  touch();
  String tag = P(r, "tag");
  String notes = P(r, "notes");
  tag.trim();
  notes.trim();
  if (!tag.length() || tag.length() > 32) { jerr(r, 400, "hazard type must be 1-32 characters"); return; }
  if (notes.length() > 240) { jerr(r, 400, "notes must be at most 240 characters"); return; }
  for (size_t i = 0; i < tag.length(); i++) {
    uint8_t c = (uint8_t)tag[i];
    if (c < 0x20 || c == 0x7f) { jerr(r, 400, "hazard type contains a control character"); return; }
  }
  uint32_t loggedAt = clockNow();
  RiskSnapshot snapshot;
  if (!riskCapture(loggedAt, snapshot)) {
    jerr(r, 503, sdOk ? "SD busy" : "SD card not available");
    return;
  }
  String encoded = riskEncodeEvent(snapshot, tag, notes);
  if (encoded.length() >= RISK_LINE_SIZE - 2) { jerr(r, 500, "hazard record is too large"); return; }
  if (!riskAppendEvent(encoded)) { jerr(r, 500, "could not save hazard record"); return; }
  riskCacheReady = false;
  JsonDocument response;
  response["ok"] = true;
  response["ts"] = loggedAt;
  sendDoc(r, response);
}

static void handleRiskDelete(Req* r) {
  if (!needAdmin(r)) return;
  if (!hasP(r, "ts") || !hasP(r, "tag")) { jerr(r, 400, "ts and tag required"); return; }
  int rc = riskDeleteEvent((uint32_t)P(r, "ts").toInt(), P(r, "tag"));
  if (rc < 0) { jerr(r, 503, sdOk ? "SD busy or write failed" : "SD card not available"); return; }
  if (rc == 0) { jerr(r, 404, "report not found"); return; }
  riskCacheReady = false;
  jok(r);
}

// ------------------------------------------------------------ file streaming
// Chunk callbacks run in the async task, so every SD touch takes sdMutex and
// returns RESPONSE_TRY_AGAIN if the logger currently owns the card.
struct FileCtx { File f; };
struct RangeCtx {
  uint32_t from, to, cur;
  File f;
  bool hdr = false, done = false;
  String pend;
  size_t pos = 0;
};

static bool validName(const String& n) { return isDataName(n); }

// Streams one daily file line by line and adds the station location as two more columns (lat,lon), so every
// downloaded CSV says where it was measured.  The file on the SD card is not changed.
struct LineCtx {
  File f;
  String pend, suffix;
  size_t pos = 0;
  bool done = false;
};

static void sendFileDownload(Req* r, const String& name) {
  if (!sdOk) { jerr(r, 503, "SD card not available"); return; }
  std::shared_ptr<LineCtx> ctx = std::make_shared<LineCtx>();
  ctx->suffix = locSuffix();
  {
    SdLock l;
    if (!l.ok) { jerr(r, 503, "SD busy"); return; }
    ctx->f = SD.open("/" + name, FILE_READ);
    if (!ctx->f) { jerr(r, 404, "no such file"); return; }
  }
  AsyncWebServerResponse* resp = r->beginChunkedResponse("text/csv",
    [ctx](uint8_t* buf, size_t maxLen, size_t index) -> size_t {
      SdLock l(0);
      if (!l.ok) return RESPONSE_TRY_AGAIN;
      size_t n = 0;
      char line[200];
      while (n < maxLen) {
        if (ctx->pos < ctx->pend.length()) {
          size_t c = min(maxLen - n, ctx->pend.length() - ctx->pos);
          memcpy(buf + n, ctx->pend.c_str() + ctx->pos, c);
          n += c; ctx->pos += c;
          continue;
        }
        ctx->pend = ""; ctx->pos = 0;
        if (ctx->done) break;
        if (!ctx->f.available()) { ctx->f.close(); ctx->done = true; break; }
        int len = ctx->f.readBytesUntil('\n', line, sizeof line - 2);
        if (len && line[len - 1] == '\r') len--;
        line[len] = 0;
        if (!len) continue;
        ctx->pend = String(line) + (strncmp(line, "epoch_utc", 9) == 0 ? String(",lat,lon") : ctx->suffix) + "\n";
      }
      return n;
    });
  resp->addHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
  r->send(resp);
}

static void nextDayStart(uint32_t& t) {
  t = (((t + (int32_t)cfg.tz_min * 60) / 86400) + 1) * 86400 - (int32_t)cfg.tz_min * 60;
}

static void sendRangeCsv(Req* r, uint32_t from, uint32_t to) {
  if (!sdOk) { jerr(r, 503, "SD card not available"); return; }
  if (to < from) std::swap(from, to);
  if (to - from > 366UL * 86400UL) to = from + 366UL * 86400UL;
  std::shared_ptr<RangeCtx> ctx = std::make_shared<RangeCtx>();
  ctx->from = from; ctx->to = to; ctx->cur = from;
  String suffix = locSuffix();
  AsyncWebServerResponse* resp = r->beginChunkedResponse("text/csv",
    [ctx, suffix](uint8_t* buf, size_t maxLen, size_t index) -> size_t {
      SdLock l(0);
      if (!l.ok) return RESPONSE_TRY_AGAIN;
      size_t n = 0;
      char line[200];
      while (n < maxLen) {
        if (ctx->pos < ctx->pend.length()) {
          size_t c = min(maxLen - n, ctx->pend.length() - ctx->pos);
          memcpy(buf + n, ctx->pend.c_str() + ctx->pos, c);
          n += c; ctx->pos += c;
          continue;
        }
        ctx->pend = ""; ctx->pos = 0;
        if (!ctx->hdr) { ctx->pend = CSV_HEADER ",lat,lon\n"; ctx->hdr = true; continue; }
        if (ctx->done) break;
        if (!ctx->f) {
          if (ctx->cur > ctx->to) { ctx->done = true; break; }
          char path[16];
          fileForEpoch(ctx->cur, path);
          nextDayStart(ctx->cur);
          ctx->f = SD.open(path, FILE_READ);
          continue;
        }
        if (!ctx->f.available()) { ctx->f.close(); continue; }
        int len = ctx->f.readBytesUntil('\n', line, sizeof line - 2);
        if (len && line[len - 1] == '\r') len--;
        line[len] = 0;
        uint32_t ts = strtoul(line, nullptr, 10);
        if (ts >= ctx->from && ts <= ctx->to) { ctx->pend = String(line) + suffix + "\n"; }
      }
      return n;
    });
  char nm[40];
  snprintf(nm, sizeof nm, "weather_%lu_%lu.csv", (unsigned long)from, (unsigned long)to);
  resp->addHeader("Content-Disposition", String("attachment; filename=\"") + nm + "\"");
  r->send(resp);
}

// ------------------------------------------------------------ history (downsampled JSON)
// Up to a year.  Long ranges are cut into at most `max` buckets: temperature, pressure, humidity, wind average and
// supply are averaged over each bucket, the gust is the bucket's highest, rain is the bucket's total, and the wind
// direction is the bucket's last reading.  A year reads about 365 daily files, so it takes several seconds.
struct Bucket {
  uint32_t ts = 0, n = 0;
  double s[5] = {0, 0, 0, 0, 0};   // temp, pres, wavg, batt, humidity
  uint32_t c[5] = {0, 0, 0, 0, 0};
  double gust = NAN, rain = 0, dir = NAN;
};

static void bucketAdd(Bucket& b, const double* v) {
  if (!b.n) b.ts = (uint32_t)v[0];
  b.n++;
  // parseRow columns: 0 epoch, 1 local time, 2 temp, 3 pres, 4 wind avg, 5 gust, 6 dir, 7 rain, 8 supply, 9 humidity
  const int col[5] = {2, 3, 4, 8, 9};
  for (int i = 0; i < 5; i++) if (!isnan(v[col[i]])) { b.s[i] += v[col[i]]; b.c[i]++; }
  if (!isnan(v[5]) && (isnan(b.gust) || v[5] > b.gust)) b.gust = v[5];
  if (!isnan(v[7])) b.rain += v[7];
  if (!isnan(v[6]) && v[6] >= 0) b.dir = v[6];
}

static void bucketPrint(Print* o, const Bucket& b, bool& first) {
  if (!b.n) return;
  auto avg = [&](int i) { return b.c[i] ? b.s[i] / b.c[i] : NAN; };
  if (!first) o->print(',');
  first = false;
  o->print('['); o->print(b.ts); o->print(',');
  pn(o, avg(0), 2); o->print(','); pn(o, avg(1), 2); o->print(','); pn(o, avg(2), 2); o->print(',');
  pn(o, b.gust, 2); o->print(','); pn(o, b.dir, 0); o->print(','); pn(o, b.rain, 3); o->print(',');
  pn(o, avg(3), 3); o->print(','); pn(o, avg(4), 1); o->print(']');
}

static void handleHistory(Req* r) {
  touch();
  if (!sdOk) { jerr(r, 503, "SD card not available"); return; }
  uint32_t now = clockNow();
  uint32_t to = r->hasParam("to") ? (uint32_t)r->getParam("to")->value().toInt() : now;
  uint32_t from = r->hasParam("from") ? (uint32_t)r->getParam("from")->value().toInt() : to - 86400;
  if (from > to) std::swap(from, to);
  if (to - from > 366UL * 86400UL) from = to - 366UL * 86400UL;
  uint32_t maxPts = r->hasParam("max") ? (uint32_t)r->getParam("max")->value().toInt() : 400;
  maxPts = constrain(maxPts, 20u, 600u);
  uint32_t iv = cfg.interval_s;
  uint32_t expected = (to - from) / iv + 1;
  uint32_t stride = max<uint32_t>(1, (expected + maxPts - 1) / maxPts);

  AsyncResponseStream* o = r->beginResponseStream("application/json");
  o->addHeader("Cache-Control", "no-store");
  o->print("{\"interval\":");
  o->print(iv);
  o->print(",\"stride\":");
  o->print(stride);
  o->print(",\"cols\":[\"ts\",\"temp\",\"pres\",\"wavg\",\"wgust\",\"wdir\",\"rain\",\"batt\",\"humidity\"],\"rows\":[");

  bool first = true;
  Bucket b;
  char line[200];
  double v[10];
  uint32_t t = from;
  for (int guard = 0; guard < 368 && t <= to; guard++) {
    char path[16];
    fileForEpoch(t, path);
    nextDayStart(t);
    SdLock l;
    if (!l.ok) break;
    File f = SD.open(path, FILE_READ);
    if (!f) continue;
    while (f.available()) {
      int len = f.readBytesUntil('\n', line, sizeof line - 1);
      line[len] = 0;
      if (!parseRow(line, v)) continue;
      if (v[0] < from || v[0] > to) continue;
      bucketAdd(b, v);
      if (b.n >= stride) { bucketPrint(o, b, first); b = Bucket(); }
    }
    f.close();
  }
  bucketPrint(o, b, first);
  o->print("]}");
  r->send(o);
}

// ------------------------------------------------------------ routes
static void webInit() {
  // ---- public ----
  server.on("/api/live", HTTP_GET, [](Req* r) {          // does NOT extend the session
    JsonDocument d;
    uint32_t now = clockNow();
    d["time"] = now;
    d["time_trusted"] = timeTrusted;
    d["interval"] = cfg.interval_s;
    d["session_left_s"] = sessionLeftS();
    d["sd_ok"] = sdOk;
    d["bmp_ok"] = bmpOk;
    d["rtc_ok"] = rtcOk;
    if (!bmpOk && bmpError) d["bmp_error"] = bmpError;
    d["rain_today"] = rainTodayMm;
    d["power_mode"] = cfg.power_mode;
    d["boot_ts"] = bootEpoch;
    d["boot_reason"] = bootReason;
    if (locationSet()) { d["lat"] = serialized(String(cfg.lat, 6)); d["lon"] = serialized(String(cfg.lon, 6)); }
    RiskAlert alert;
    if (riskCurrentAlert(alert)) {
      d["risk_level"] = alert.level;
      if (alert.level != "none") {
        d["risk_tag"] = alert.tag;
        d["risk_score"] = alert.score;
        d["risk_window_hours"] = alert.windowHours;
      }
    } else {
      d["risk_error"] = sdOk ? "SD busy" : "SD card not available";
    }
    JsonObject nw = d["now"].to<JsonObject>();
    nw["wind"] = liveWind;
    if (liveDir >= 0) nw["dir"] = liveDir;
    if (!isnan(liveTemp)) nw["temp"] = liveTemp;
    if (!isnan(livePres)) nw["pressure"] = livePres;
    if (!isnan(liveHumidity)) nw["humidity"] = liveHumidity;
    if (!isnan(liveBatt)) nw["batt"] = liveBatt;
    if (haveRec) {
      JsonObject l = d["last"].to<JsonObject>();
      l["ts"] = lastRec.ts;
      if (!isnan(lastRec.temp)) l["temp"] = lastRec.temp;
      if (!isnan(lastRec.pres)) l["pressure"] = lastRec.pres;
      if (!isnan(lastRec.humidity)) l["humidity"] = lastRec.humidity;
      if (!isnan(lastRec.wavg)) l["wind_avg"] = lastRec.wavg;
      if (!isnan(lastRec.wgust)) l["wind_gust"] = lastRec.wgust;
      if (lastRec.wdir >= 0) l["wind_dir"] = lastRec.wdir;
      if (!isnan(lastRec.rain)) l["rain"] = lastRec.rain;
      if (!isnan(lastRec.batt)) l["batt"] = lastRec.batt;
    }
    sendDoc(r, d);
  });

  server.on("/api/risks", HTTP_GET, handleRiskList);
  server.on("/api/risks/log", HTTP_POST, handleRiskLog);
  server.on("/api/risks/delete", HTTP_POST, handleRiskDelete);
  server.on("/api/history", HTTP_GET, handleHistory);

  server.on("/api/files", HTTP_GET, [](Req* r) {
    touch();
    JsonDocument d;
    JsonArray a = d["files"].to<JsonArray>();
    for (auto& f : sdList()) {
      JsonObject o = a.add<JsonObject>();
      o["name"] = f.name;
      o["size"] = f.size;
    }
    if (sdOk) {
      SdLock l;
      if (l.ok) { d["total"] = (uint64_t)SD.totalBytes(); d["used"] = (uint64_t)SD.usedBytes(); }
    }
    d["sd_ok"] = sdOk;
    sendDoc(r, d);
  });

  server.on("/api/file", HTTP_GET, [](Req* r) {
    touch();
    String n = r->hasParam("name") ? r->getParam("name")->value() : "";
    if (!validName(n)) { jerr(r, 400, "bad file name"); return; }
    sendFileDownload(r, n);
  });

  server.on("/api/range.csv", HTTP_GET, [](Req* r) {
    touch();
    if (!r->hasParam("from") || !r->hasParam("to")) { jerr(r, 400, "from/to required"); return; }
    sendRangeCsv(r, (uint32_t)r->getParam("from")->value().toInt(),
                 (uint32_t)r->getParam("to")->value().toInt());
  });

  server.on("/api/keepalive", HTTP_POST, [](Req* r) { touch(); jok(r); });

  server.on("/api/net", HTTP_GET, [](Req* r) {
    touch();
    JsonDocument d;
    d["host"] = cfg.host + ".local";
    d["ap_ip"] = WiFi.softAPIP().toString();
    d["sta_connected"] = staConnected();
    if (staConnected()) {
      d["sta_ip"] = WiFi.localIP().toString();
      d["ssid"] = WiFi.SSID();
      d["rssi"] = WiFi.RSSI();
    }
    d["saved_ssid"] = [] { String s; staHasCreds(&s); return s; }();
    sendDoc(r, d);
  });

  // ---- session / auth ----
  server.on("/api/session", HTTP_GET, [](Req* r) {
    touch();
    JsonDocument d;
    d["admin"] = authCheck(r);
    d["password_set"] = authHasPassword();
    d["setup_open"] = setupOpen();
    d["locked"] = authLocked();
    d["time_trusted"] = timeTrusted;
    if (resetMsg.length()) d["reset_msg"] = resetMsg;
    sendDoc(r, d);
  });

  auto giveCookie = [](Req* r, const String& tok) {
    AsyncWebServerResponse* resp = r->beginResponse(200, "application/json", "{\"ok\":true}");
    resp->addHeader("Set-Cookie", "sid=" + tok + "; Path=/; HttpOnly; SameSite=Strict; Max-Age=1800");
    r->send(resp);
  };

  server.on("/api/login", HTTP_POST, [giveCookie](Req* r) {
    touch();
    String tok;
    int rc = authLogin(P(r, "password"), tok);
    if (rc == 2) { jerr(r, 429, "too many attempts, wait 5 minutes"); return; }
    if (rc == 1) { jerr(r, 403, "wrong password"); return; }
    giveCookie(r, tok);
  });

  server.on("/api/logout", HTTP_POST, [](Req* r) {
    authLogout(r);
    AsyncWebServerResponse* resp = r->beginResponse(200, "application/json", "{\"ok\":true}");
    resp->addHeader("Set-Cookie", "sid=; Path=/; Max-Age=0");
    r->send(resp);
  });

  server.on("/api/setup", HTTP_POST, [giveCookie](Req* r) {   // first run or after hardware reset
    touch();
    if (!setupOpen()) { jerr(r, 403, "setup window closed"); return; }
    String pw = P(r, "password");
    if (pw.length() < 8 || pw.length() > 64) { jerr(r, 400, "password must be 8-64 characters"); return; }
    authSetPassword(pw);
    setupOpenUntil = 0;
    resetMsg = "";
    String tok;
    authLogin(pw, tok);
    giveCookie(r, tok);
  });

  server.on("/api/password", HTTP_POST, [](Req* r) {
    if (!needAdmin(r)) return;
    if (!authVerify(P(r, "old"))) { jerr(r, 403, "current password is wrong"); return; }
    String pw = P(r, "new");
    if (pw.length() < 8 || pw.length() > 64) { jerr(r, 400, "password must be 8-64 characters"); return; }
    authSetPassword(pw);
    jok(r);
  });

  // ---- admin: Wi-Fi ----
  server.on("/api/wifi/scan", HTTP_GET, [](Req* r) {
    if (!needAdmin(r)) return;
    JsonDocument d;
    if (r->hasParam("start")) {
      WiFi.scanDelete();
      WiFi.scanNetworks(true);
      d["scanning"] = true;
      sendDoc(r, d);
      return;
    }
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) { d["scanning"] = true; sendDoc(r, d); return; }
    if (n < 0) { d["scanning"] = false; d["networks"].to<JsonArray>(); sendDoc(r, d); return; }
    d["scanning"] = false;
    JsonArray a = d["networks"].to<JsonArray>();
    for (int i = 0; i < n && i < 30; i++) {
      JsonObject o = a.add<JsonObject>();
      o["ssid"] = WiFi.SSID(i);
      o["rssi"] = WiFi.RSSI(i);
      o["open"] = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
    }
    WiFi.scanDelete();
    sendDoc(r, d);
  });

  server.on("/api/wifi/save", HTTP_POST, [](Req* r) {
    if (!needAdmin(r)) return;
    String ssid = P(r, "ssid"), pass = P(r, "pass");
    if (ssid.length() > 32) { jerr(r, 400, "SSID too long"); return; }
    if (pass.length() && (pass.length() < 8 || pass.length() > 63)) {
      jerr(r, 400, "Wi-Fi password must be 8-63 characters (or empty for open networks)");
      return;
    }
    Preferences p;
    p.begin("net", false);
    p.putString("ssid", ssid);
    p.putString("pass", pass);
    p.end();
    if (ssid.length()) { WiFi.disconnect(); WiFi.begin(ssid.c_str(), pass.c_str()); }
    jok(r);
  });

  // ---- admin: config / time ----
  server.on("/api/config", HTTP_GET, [](Req* r) {
    if (!needAdmin(r)) return;
    JsonDocument d;
    d["interval_s"] = cfg.interval_s;
    d["window_s"] = cfg.window_s;
    d["wind_k"] = cfg.wind_k;
    d["rain_mm_tip"] = cfg.rain_mm_tip;
    d["altitude_m"] = cfg.altitude_m;
    d["tz_min"] = cfg.tz_min;
    d["vane_off"] = cfg.vane_off;
    d["session_s"] = cfg.session_s;
    d["power_mode"] = cfg.power_mode;
    d["batt_min_wifi"] = cfg.batt_min_wifi;
    d["batt_min_upload"] = cfg.batt_min_upload;
    d["ap_ssid"] = cfg.ap_ssid;
    d["up_en"] = cfg.up_en;
    d["up_url"] = cfg.up_url;
    d["up_tok_set"] = cfg.up_tok.length() > 0;
    d["up_int_min"] = cfg.up_int_min;
    d["up_insecure"] = cfg.up_insecure;
    d["ca_set"] = LittleFS.exists("/ca.pem");
    if (locationSet()) { d["lat"] = serialized(String(cfg.lat, 6)); d["lon"] = serialized(String(cfg.lon, 6)); }
    sendDoc(r, d);
  });

  server.on("/api/config", HTTP_POST, [](Req* r) {
    if (!needAdmin(r)) return;
    auto num = [&](const char* k, double lo, double hi, double cur) -> double {
      return hasP(r, k) ? constrain(P(r, k).toDouble(), lo, hi) : cur;
    };
    cfg.interval_s = (uint32_t)num("interval_s", 60, 86400, cfg.interval_s);
    cfg.window_s = (uint16_t)num("window_s", 3, 60, cfg.window_s);
    if (cfg.window_s * 2UL > cfg.interval_s) cfg.window_s = cfg.interval_s / 2;
    cfg.wind_k = num("wind_k", 0.01, 20, cfg.wind_k);
    cfg.rain_mm_tip = num("rain_mm_tip", 0.001, 10, cfg.rain_mm_tip);
    cfg.altitude_m = num("altitude_m", -400, 9000, cfg.altitude_m);
    cfg.tz_min = (int16_t)num("tz_min", -720, 840, cfg.tz_min);
    cfg.vane_off = (int16_t)num("vane_off", -359, 359, cfg.vane_off);
    cfg.session_s = (uint16_t)num("session_s", 120, 3600, cfg.session_s);
    cfg.power_mode = (uint8_t)num("power_mode", POWER_USB, POWER_BATTERY, cfg.power_mode);
    cfg.batt_min_wifi = num("batt_min_wifi", 3.0, 4.2, cfg.batt_min_wifi);
    cfg.batt_min_upload = num("batt_min_upload", 3.0, 4.2, cfg.batt_min_upload);
    cfg.up_int_min = (uint16_t)num("up_int_min", 5, 1440, cfg.up_int_min);
    if (hasP(r, "ap_ssid") && P(r, "ap_ssid").length() >= 1 && P(r, "ap_ssid").length() <= 32) cfg.ap_ssid = P(r, "ap_ssid");
    if (hasP(r, "ap_pass") && P(r, "ap_pass").length() >= 8 && P(r, "ap_pass").length() <= 63) cfg.ap_pass = P(r, "ap_pass");
    if (hasP(r, "up_en")) cfg.up_en = P(r, "up_en") == "1";
    if (hasP(r, "up_insecure")) cfg.up_insecure = P(r, "up_insecure") == "1";
    if (hasP(r, "up_url")) {
      String u = P(r, "up_url");
      u.trim();
      if (u.length() && !u.startsWith("https://")) { jerr(r, 400, "upload URL must start with https://"); return; }
      cfg.up_url = u;
    }
    if (hasP(r, "up_tok") && P(r, "up_tok").length()) cfg.up_tok = P(r, "up_tok");
    if (P(r, "clear_tok") == "1") cfg.up_tok = "";
    cfgSave();
    nextUpload = 0;
    rescheduleReq = true;     // controller re-arms the RTC alarm (it owns I2C)
    jok(r);
  });

  server.on("/api/time", HTTP_POST, [](Req* r) {
    if (!needAdmin(r)) return;
    uint32_t t = (uint32_t)P(r, "epoch").toInt();
    if (t < 1735689600UL || t > 4102444800UL) { jerr(r, 400, "bad time"); return; }
    pendingEpoch = t;       // applied by the controller task (owns the I2C bus)
    jok(r);
  });

  // ---- admin: station location (GPS) ----
  server.on("/api/location", HTTP_POST, [](Req* r) {
    if (!needAdmin(r)) return;
    if (P(r, "clear") == "1") { cfg.lat = NAN; cfg.lon = NAN; cfgSave(); jok(r); return; }
    String la = P(r, "lat"), lo = P(r, "lon");
    la.trim(); lo.trim();
    char* e1 = nullptr; char* e2 = nullptr;
    double lat = strtod(la.c_str(), &e1), lon = strtod(lo.c_str(), &e2);
    if (!la.length() || !lo.length() || *e1 || *e2 || isnan(lat) || isnan(lon) ||
        lat < -90 || lat > 90 || lon < -180 || lon > 180) {
      jerr(r, 400, "latitude must be -90 to 90 and longitude -180 to 180 (decimal degrees)");
      return;
    }
    cfg.lat = lat; cfg.lon = lon;
    cfgSave();
    jok(r);
  });

  // ---- admin: upload ----
  server.on("/api/upload/test", HTTP_POST, [](Req* r) {
    if (!needAdmin(r)) return;
    uploadStatus = "testing...";
    uploadTestReq = true;
    jok(r);
  });
  server.on("/api/upload/now", HTTP_POST, [](Req* r) {
    if (!needAdmin(r)) return;
    uploadStatus = "uploading...";
    uploadNowReq = true;
    jok(r);
  });
  server.on("/api/upload/status", HTTP_GET, [](Req* r) {
    if (!needAdmin(r)) return;
    JsonDocument d;
    d["status"] = uploadStatus;
    d["sync_file"] = syncFileInfo;
    d["sync_offset"] = syncOffInfo;
    d["next_upload"] = nextUpload;
    sendDoc(r, d);
  });
  server.on("/api/upload/ca", HTTP_POST, [](Req* r) {
    if (!needAdmin(r)) return;
    String pem = P(r, "pem");
    pem.trim();
    if (pem.length() == 0) { LittleFS.remove("/ca.pem"); jok(r); return; }
    if (pem.indexOf("BEGIN CERTIFICATE") < 0 || pem.length() > 8000) { jerr(r, 400, "not a PEM certificate"); return; }
    File f = LittleFS.open("/ca.pem", "w");
    if (!f) { jerr(r, 500, "write failed"); return; }
    f.print(pem);
    f.print("\n");
    f.close();
    jok(r);
  });

  server.on("/api/sleep", HTTP_POST, [](Req* r) {
    if (!needAdmin(r)) return;
    endRequested = true;
    jok(r);
  });

  // ---- static UI (only /www is exposed; /ca.pem etc. are not) ----
  server.serveStatic("/", LittleFS, "/www/").setDefaultFile("index.html").setCacheControl("max-age=300");

  server.onNotFound([](Req* r) {
    if (r->url().startsWith("/api/")) { jerr(r, 404, "not found"); return; }
    r->send(404, "text/plain", "Page not found. Upload the LittleFS filesystem image.");
  });
}
