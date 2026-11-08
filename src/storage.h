#pragma once
// SD card: daily CSV logging, tail repair, listing. All access goes through sdMutex.
#include <SD.h>
#include <SPI.h>
#include <vector>
#include <algorithm>
#include "state.h"

struct SdLock {
  bool ok;
  explicit SdLock(uint32_t ms = 10000) { ok = xSemaphoreTake(sdMutex, pdMS_TO_TICKS(ms)) == pdTRUE; }
  ~SdLock() { if (ok) xSemaphoreGive(sdMutex); }
};

struct FileInfo {
  String name;
  uint32_t size;
};

static bool sdInit() {
  SdLock l;
  if (!l.ok) return false;
  SD.end();
  SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  sdOk = SD.begin(PIN_SD_CS, SPI, 10000000);
  return sdOk;
}

static bool isDataName(const String& n) {  // YYYY-MM-DD.csv
  if (n.length() != 14) return false;
  for (int i = 0; i < 10; i++) {
    if (i == 4 || i == 7) { if (n[i] != '-') return false; }
    else if (!isDigit(n[i])) return false;
  }
  return n.endsWith(".csv");
}

static bool sdRecoverCsvSchema() {
  if (!sdOk) return false;
  SdLock l;
  if (!l.ok) return false;

  const char* metaPath = "/csvmeta";
  const char* tempPath = "/csvnew";
  const char* backupPath = "/csvold";
  if (!SD.exists(metaPath)) {
    if (SD.exists(tempPath)) SD.remove(tempPath);
    return true;
  }

  File meta = SD.open(metaPath, FILE_READ);
  if (!meta) return false;
  char path[16];
  int n = meta.readBytesUntil('\n', path, sizeof(path) - 1);
  path[n] = 0;
  meta.close();
  String name(path);
  if (!name.startsWith("/") || !isDataName(name.substring(1))) return false;

  if (SD.exists(path)) {
    if (SD.exists(backupPath)) SD.remove(backupPath);
    if (SD.exists(tempPath)) SD.remove(tempPath);
    SD.remove(metaPath);
    return true;
  }
  if (SD.exists(tempPath)) {
    if (!SD.rename(tempPath, path)) return false;
    if (SD.exists(backupPath)) SD.remove(backupPath);
  } else if (SD.exists(backupPath)) {
    if (!SD.rename(backupPath, path)) return false;
  } else {
    return false;
  }
  SD.remove(metaPath);
  return true;
}

static bool sdUpgradeCsvSchema(const char* path) {
  File src = SD.open(path, FILE_READ);
  if (!src) return false;
  char header[160];
  int n = src.readBytesUntil('\n', header, sizeof(header) - 1);
  header[n] = 0;
  if (n && header[n - 1] == '\r') header[n - 1] = 0;
  if (strcmp(header, CSV_HEADER) == 0) { src.close(); return true; }
  if (strcmp(header, CSV_HEADER_V1) != 0) { src.close(); return false; }

  const char* metaPath = "/csvmeta";
  const char* tempPath = "/csvnew";
  const char* backupPath = "/csvold";
  SD.remove(tempPath);
  File dst = SD.open(tempPath, FILE_WRITE);
  if (!dst) { src.close(); return false; }
  dst.print(CSV_HEADER "\n");
  uint8_t buf[512];
  bool copied = dst.getWriteError() == 0;
  while (copied && src.available()) {
    size_t count = src.read(buf, sizeof(buf));
    if (count && dst.write(buf, count) != count) copied = false;
  }
  dst.flush();
  copied = copied && dst.getWriteError() == 0;
  src.close();
  dst.close();
  if (!copied) { SD.remove(tempPath); return false; }

  File meta = SD.open(metaPath, FILE_WRITE);
  if (!meta) { SD.remove(tempPath); return false; }
  meta.print(path);
  meta.print('\n');
  meta.flush();
  bool metaOk = meta.getWriteError() == 0;
  meta.close();
  if (!metaOk) { SD.remove(metaPath); SD.remove(tempPath); return false; }

  SD.remove(backupPath);
  if (!SD.rename(path, backupPath)) {
    SD.remove(metaPath);
    SD.remove(tempPath);
    return false;
  }
  if (!SD.rename(tempPath, path)) {
    SD.rename(backupPath, path);
    SD.remove(metaPath);
    SD.remove(tempPath);
    return false;
  }
  SD.remove(backupPath);
  SD.remove(metaPath);
  return true;
}

// Parse one CSV row (modified in place). f[1] (local_iso) is unused.
static bool parseRow(char* line, double f[10]) {
  size_t L = strlen(line);
  while (L && (line[L - 1] == '\r' || line[L - 1] == '\n')) line[--L] = 0;
  for (int i = 0; i < 10; i++) f[i] = NAN;
  int i = 0;
  char* p = line;
  while (i < 10) {
    char* c = strchr(p, ',');
    if (c) *c = 0;
    f[i] = (i == 1) ? 0 : (*p ? atof(p) : NAN);
    i++;
    if (!c) break;
    p = c + 1;
  }
  return (i == 9 || i == 10) && f[0] > 1000000000.0;
}

static void addNum(String& s, float v, int dec) {
  if (!isnan(v)) s += String(v, dec);
}

static bool sdLog(const Rec& r) {
  if (!sdOk && !sdInit()) return false;
  SdLock l;
  if (!l.ok) return false;
  char path[16];
  fileForEpoch(r.ts, path);
  bool isNew = !SD.exists(path);
  if (!isNew && !sdUpgradeCsvSchema(path)) return false;
  File f = SD.open(path, FILE_APPEND);
  if (!f) { sdOk = false; return false; }
  if (isNew) f.print(CSV_HEADER "\n");
  char iso[32];
  fmtLocalIso(r.ts, iso, sizeof iso);
  String s;
  s.reserve(120);
  s += r.ts; s += ','; s += iso; s += ',';
  addNum(s, r.temp, 2); s += ',';
  addNum(s, r.pres, 2); s += ',';
  addNum(s, r.wavg, 2); s += ',';
  addNum(s, r.wgust, 2); s += ',';
  if (r.wdir >= 0) s += r.wdir;
  s += ',';
  addNum(s, r.rain, 3); s += ',';
  addNum(s, r.batt, 3); s += ',';
  addNum(s, r.humidity, 1); s += '\n';
  size_t w = f.print(s);
  f.flush();            // push to the card immediately
  f.close();
  if (w != s.length()) { sdOk = false; return false; }
  return true;
}

// After a brown-out the last row may be cut off. Make sure the file ends in '\n'
// so the next append starts on a fresh line (parseRow rejects the broken row).
static void sdRepairTail(const char* path) {
  SdLock l;
  if (!l.ok || !sdOk) return;
  File f = SD.open(path, FILE_READ);
  if (!f) return;
  size_t sz = f.size();
  int c = '\n';
  if (sz) { f.seek(sz - 1); c = f.read(); }
  f.close();
  if (c != '\n') {
    File a = SD.open(path, FILE_APPEND);
    if (a) { a.print("\n"); a.flush(); a.close(); }
  }
}

// At boot: restore today's rain total and the last record from today's file.
static void sdSeedToday() {
  if (!sdOk) return;
  char path[16];
  fileForEpoch(clockNow(), path);
  sdRepairTail(path);
  SdLock l;
  if (!l.ok) return;
  File f = SD.open(path, FILE_READ);
  if (!f) return;
  char line[200];
  double v[10];
  double rain = 0;
  while (f.available()) {
    int n = f.readBytesUntil('\n', line, sizeof line - 1);
    line[n] = 0;
    if (!parseRow(line, v)) continue;
    if (!isnan(v[7])) rain += v[7];
    lastRec.ts = (uint32_t)v[0];
    lastRec.temp = v[2]; lastRec.pres = v[3];
    lastRec.wavg = v[4]; lastRec.wgust = v[5];
    lastRec.wdir = isnan(v[6]) ? -1 : (int16_t)v[6];
    lastRec.rain = v[7]; lastRec.batt = v[8];
    lastRec.humidity = v[9];
    haveRec = true;
  }
  f.close();
  rainTodayMm = rain;
}

static std::vector<FileInfo> sdList() {
  std::vector<FileInfo> v;
  if (!sdOk) return v;
  SdLock l;
  if (!l.ok) return v;
  File root = SD.open("/");
  if (!root) return v;
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    if (!f.isDirectory()) {
      String n = f.name();
      if (n.startsWith("/")) n.remove(0, 1);
      if (isDataName(n)) v.push_back({n, (uint32_t)f.size()});
    }
    f.close();
  }
  root.close();
  std::sort(v.begin(), v.end(), [](const FileInfo& a, const FileInfo& b) { return a.name < b.name; });
  return v;
}
