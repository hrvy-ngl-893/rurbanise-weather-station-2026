#pragma once
// Optional remote upload. Progress is tracked in /sync.json on the SD card as
// {"file":"YYYY-MM-DD.csv","offset":<bytes>}. The pointer only advances after a 2xx
// response, so a failed or half-finished upload is simply retried from the same place.
// Each POST carries X-Station-File and X-Start-Offset so the server can de-duplicate
// if a response is lost after the data was accepted.
// When a station location is set, X-Station-Lat / X-Station-Lon (and lat= / lon= URL parameters) carry it.
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <memory>
#include "state.h"
#include "storage.h"
#include "session.h"

struct SyncPtr {
  String file;
  uint32_t off = 0;
};
static SyncPtr syncPtr;
static uint8_t uploadFails = 0;
static bool uploadMore = false;

static void syncLoad() {
  if (!sdOk) return;
  SdLock l;
  if (!l.ok) return;
  File f = SD.open("/sync.json", FILE_READ);
  if (!f) f = SD.open("/sync.tmp", FILE_READ);   // power cut between remove and rename
  if (!f) return;
  JsonDocument d;
  if (!deserializeJson(d, f)) {
    syncPtr.file = d["file"] | "";
    syncPtr.off = d["offset"] | 0;
  }
  f.close();
  syncFileInfo = syncPtr.file;
  syncOffInfo = syncPtr.off;
}

static bool syncSave() {
  SdLock l;
  if (!l.ok) return false;
  File f = SD.open("/sync.tmp", FILE_WRITE);
  if (!f) return false;
  JsonDocument d;
  d["file"] = syncPtr.file;
  d["offset"] = syncPtr.off;
  serializeJson(d, f);
  f.flush();
  f.close();
  SD.remove("/sync.json");
  SD.rename("/sync.tmp", "/sync.json");
  syncFileInfo = syncPtr.file;
  syncOffInfo = syncPtr.off;
  return true;
}

// Find the next chunk of unsent CSV data. Never sends a partial last row.
static bool nextBatch(String& body, String& fname, uint32_t& off, uint32_t& newOff, String& newFile) {
  std::vector<FileInfo> files = sdList();
  if (files.empty()) return false;
  char today[12];
  fmtDate(clockNow(), today);
  String todayName = String(today) + ".csv";

  int idx = -1;
  uint32_t o = syncPtr.off;
  for (size_t i = 0; i < files.size(); i++)
    if (syncPtr.file.length() && files[i].name == syncPtr.file) { idx = (int)i; break; }
  if (idx < 0) {                       // no pointer yet, or its file was deleted
    o = 0;
    for (size_t i = 0; i < files.size(); i++)
      if (files[i].name > syncPtr.file) { idx = (int)i; break; }
    if (idx < 0) return false;
  }

  while (idx < (int)files.size()) {
    const FileInfo& fi = files[idx];
    if (o >= fi.size) {
      if (fi.name == todayName) return false;   // caught up with the live file
      idx++; o = 0;
      continue;
    }
    size_t want = min<size_t>(UPLOAD_BATCH_BYTES, fi.size - o);
    std::unique_ptr<char[]> buf(new (std::nothrow) char[want + 1]);
    if (!buf) return false;
    size_t n = 0;
    {
      SdLock l;
      if (!l.ok) return false;
      File f = SD.open("/" + fi.name, FILE_READ);
      if (!f) return false;
      f.seek(o);
      n = f.read((uint8_t*)buf.get(), want);
      f.close();
    }
    if (n == 0) return false;
    int last = -1;
    for (int i = (int)n - 1; i >= 0; i--) if (buf[i] == '\n') { last = i; break; }
    if (last < 0) {
      if (fi.name == todayName) return false;   // partial row, wait for the rest
      last = (int)n - 1;                        // malformed old file: send as is
    }
    buf[last + 1] = 0;
    body = "";                                          // every row gets the station location as lat,lon columns
    String suffix = locSuffix();
    for (char* p = buf.get(); *p;) {
      char* e = strchr(p, '\n');
      size_t L = e ? (size_t)(e - p) : strlen(p);
      if (L && p[L - 1] == '\r') L--;
      if (L) {
        body.concat(p, L);
        body += strncmp(p, "epoch_utc", 9) == 0 ? String(",lat,lon") : suffix;
        body += '\n';
      }
      if (!e) break;
      p = e + 1;
    }
    fname = fi.name;
    off = o;
    newOff = o + last + 1;
    newFile = fi.name;
    return true;
  }
  return false;
}

static bool httpPost(const String& body, const String& fname, uint32_t off, bool test, String& msg) {
  if (!cfg.up_insecure && !timeTrusted) { msg = "clock not set, cannot verify TLS"; return false; }
  WiFiClientSecure client;
  String ca;
  if (cfg.up_insecure) {
    client.setInsecure();
  } else {
    File f = LittleFS.open("/ca.pem", "r");
    if (f) { ca = f.readString(); f.close(); }
    if (ca.length() < 100) { msg = "no CA certificate (paste PEM in admin, or enable insecure debug)"; return false; }
    client.setCACert(ca.c_str());
  }
  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  String url = cfg.up_url;
  if (locationSet())                    // also as URL parameters: some receivers (Google Apps Script) cannot read headers
    url += String(url.indexOf('?') >= 0 ? "&" : "?") + "lat=" + String(cfg.lat, 6) + "&lon=" + String(cfg.lon, 6);
  if (!http.begin(client, url)) { msg = "bad URL"; return false; }
  http.addHeader("Content-Type", "text/csv");
  if (cfg.up_tok.length()) http.addHeader("Authorization", "Bearer " + cfg.up_tok);
  http.addHeader("X-Station-Id", cfg.host);
  if (locationSet()) {
    http.addHeader("X-Station-Lat", String(cfg.lat, 6));
    http.addHeader("X-Station-Lon", String(cfg.lon, 6));
  }
  if (test) {
    http.addHeader("X-Station-Test", "1");
  } else {
    http.addHeader("X-Station-File", fname);
    http.addHeader("X-Start-Offset", String(off));
  }
  int code = http.POST((uint8_t*)body.c_str(), body.length());
  http.end();
  msg = code > 0 ? "HTTP " + String(code) : HTTPClient::errorToString(code);
  return code >= 200 && code < 300;
}

static bool uploadRun(bool testOnly, String& msg) {
  uploadMore = false;
  if (testOnly) return httpPost("ping\n", "", 0, true, msg);
  uint32_t sent = 0;
  for (int b = 0; b < UPLOAD_MAX_BATCHES; b++) {
    String body, fname, newFile;
    uint32_t off, newOff;
    if (!nextBatch(body, fname, off, newOff, newFile)) {
      msg = sent ? "uploaded " + String(sent) + " bytes" : "up to date";
      return true;
    }
    if (!httpPost(body, fname, off, false, msg)) return false;
    syncPtr.file = newFile;
    syncPtr.off = newOff;
    syncSave();
    sent += body.length();
    rainService();
  }
  uploadMore = true;
  msg = "uploaded " + String(sent) + " bytes (more pending)";
  return true;
}

static void uploadTick(uint32_t now) {
  bool test = uploadTestReq, force = uploadNowReq;
  bool due = cfg.up_en && cfg.up_url.length() && now >= nextUpload;
  if (!(test || force || due)) return;
  uploadTestReq = false;
  uploadNowReq = false;
  uint32_t period = (uint32_t)cfg.up_int_min * 60UL;

  if (!cfg.up_url.startsWith("https://")) {
    uploadStatus = "set an https:// URL first";
    if (!test && !force) nextUpload = now + 3600;
    return;
  }
  if (!test && !force && cfg.power_mode == POWER_BATTERY && !isnan(lastRec.batt) && lastRec.batt < cfg.batt_min_upload) {
    uploadStatus = "skipped: battery low";
    nextUpload = now + period;
    return;
  }

  bool temp = false;
  if (!staConnected()) {
    if (!staHasCreds()) {
      uploadStatus = "no Wi-Fi network saved";
      if (!test && !force) nextUpload = now + period;
      return;
    }
    if (!sessionActive) { WiFi.mode(WIFI_STA); temp = true; }
    staBegin();
    uint32_t t0 = millis();
    while (!staConnected() && millis() - t0 < 20000) { rainService(); delay(100); }
  }

  String msg;
  bool ok = false;
  if (!staConnected()) {
    msg = "Wi-Fi connect failed";
  } else {
    if (!timeTrusted || !ntpStarted) {            // get a trusted clock for TLS
      uint32_t t0 = millis();
      while (!ntpTick() && millis() - t0 < 8000) { rainService(); delay(100); }
    }
    ok = uploadRun(test, msg);
  }
  if (temp) { WiFi.disconnect(true); WiFi.mode(WIFI_OFF); ntpStarted = false; }

  if (test) {
    uploadStatus = String(ok ? "test OK: " : "test failed: ") + msg;
    return;
  }
  uploadStatus = String(ok ? "ok: " : "failed: ") + msg;
  if (ok) {
    uploadFails = 0;
    nextUpload = now + (uploadMore ? 60 : period);
  } else {
    if (uploadFails < 10) uploadFails++;
    uint32_t back = 60UL << min<int>(uploadFails, 10);   // 2,4,8... minutes
    nextUpload = now + min<uint32_t>(period, back);
  }
}
