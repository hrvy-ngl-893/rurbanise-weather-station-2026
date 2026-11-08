#pragma once
// Hazard events are stored separately from the weather CSV so existing data stays unchanged.
#include <ArduinoJson.h>
#include "state.h"
#include "storage.h"
#include "sensors.h"

static const char* RISK_FILE = "/hazards.jsonl";
static const char* RISK_TMP = "/hazards.tmp";
static const uint8_t RISK_WINDOWS = 3;
static const uint8_t RISK_LIST_LIMIT = 20;
static const size_t RISK_LINE_SIZE = 2048;
static const uint32_t RISK_WINDOW_SECONDS[RISK_WINDOWS] = {10800, 21600, 43200};

struct RiskWindow {
  float rain = 0;
  float temp = NAN, pressure = NAN, windAvg = NAN, windGust = NAN, humidity = NAN;
  float tempChange = NAN, pressureChange = NAN;
};

struct RiskSnapshot {
  uint32_t ts = 0;
  float temp = NAN, pressure = NAN, humidity = NAN, wind = NAN, direction = NAN, battery = NAN;
  RiskWindow windows[RISK_WINDOWS];
};

struct RiskAccumulator {
  double temp = 0, pressure = 0, wind = 0, humidity = 0;
  uint16_t tempCount = 0, pressureCount = 0, windCount = 0, humidityCount = 0;
  float rain = 0, gust = NAN;
  float firstTemp = NAN, lastTemp = NAN, firstPressure = NAN, lastPressure = NAN;
};

struct RiskAlert {
  String level = "none";
  String tag;
  float score = 0;
  uint8_t windowHours = 0;
};

static RiskAlert cachedRiskAlert;
static uint32_t riskCacheMs = 0;
static bool riskCacheReady = false;

static void riskAccumulate(RiskAccumulator& a, const double row[10], bool includeRain) {
  if (!isnan(row[2])) {
    if (isnan(a.firstTemp)) a.firstTemp = row[2];
    a.lastTemp = row[2];
    a.temp += row[2];
    a.tempCount++;
  }
  if (!isnan(row[3])) {
    if (isnan(a.firstPressure)) a.firstPressure = row[3];
    a.lastPressure = row[3];
    a.pressure += row[3];
    a.pressureCount++;
  }
  if (!isnan(row[4])) { a.wind += row[4]; a.windCount++; }
  if (!isnan(row[5]) && (isnan(a.gust) || row[5] > a.gust)) a.gust = row[5];
  if (includeRain && !isnan(row[7])) a.rain += row[7];
  if (!isnan(row[9])) { a.humidity += row[9]; a.humidityCount++; }
}

static void riskAccumulateLive(RiskAccumulator& a, const RiskSnapshot& s) {
  if (!isnan(s.temp)) {
    if (isnan(a.firstTemp)) a.firstTemp = s.temp;
    a.lastTemp = s.temp;
    a.temp += s.temp;
    a.tempCount++;
  }
  if (!isnan(s.pressure)) {
    if (isnan(a.firstPressure)) a.firstPressure = s.pressure;
    a.lastPressure = s.pressure;
    a.pressure += s.pressure;
    a.pressureCount++;
  }
  if (!isnan(s.wind)) { a.wind += s.wind; a.windCount++; }
  if (!isnan(s.humidity)) { a.humidity += s.humidity; a.humidityCount++; }
}

static bool riskCapture(uint32_t now, RiskSnapshot& out) {
  out = RiskSnapshot();
  out.ts = now;
  out.temp = isnan(liveTemp) ? (haveRec ? lastRec.temp : NAN) : liveTemp;
  out.pressure = isnan(livePres) ? (haveRec ? lastRec.pres : NAN) : livePres;
  out.humidity = isnan(liveHumidity) ? (haveRec ? lastRec.humidity : NAN) : liveHumidity;
  out.wind = liveWind;
  out.direction = liveDir < 0 ? NAN : liveDir;
  out.battery = isnan(liveBatt) ? (haveRec ? lastRec.batt : NAN) : liveBatt;

  RiskAccumulator acc[RISK_WINDOWS];
  const uint32_t earliest = now - RISK_WINDOW_SECONDS[RISK_WINDOWS - 1];
  if (!sdOk) return false;
  {
    SdLock lock;
    if (!lock.ok) return false;
    const uint32_t searchStart = earliest > cfg.interval_s ? earliest - cfg.interval_s : 0;
    uint32_t day = searchStart;
    uint32_t previousTs = 0;
    bool havePrevious = false;
    for (int guard = 0; guard < 3 && day <= now; guard++) {
      char path[16];
      fileForEpoch(day, path);
      uint32_t local = day + (int32_t)cfg.tz_min * 60;
      day = ((local / 86400) + 1) * 86400 - (int32_t)cfg.tz_min * 60;
      File f = SD.open(path, FILE_READ);
      if (!f) continue;
      char line[200];
      double row[10];
      while (f.available()) {
        int n = f.readBytesUntil('\n', line, sizeof(line) - 1);
        line[n] = 0;
        if (!parseRow(line, row) || row[0] < searchStart || row[0] > now) continue;
        for (uint8_t i = 0; i < RISK_WINDOWS; i++) {
          uint32_t cutoff = now - RISK_WINDOW_SECONDS[i];
          if (row[0] >= cutoff)
            riskAccumulate(acc[i], row, havePrevious && previousTs >= cutoff);
        }
        previousTs = (uint32_t)row[0];
        havePrevious = true;
      }
      f.close();
    }
  }

  for (uint8_t i = 0; i < RISK_WINDOWS; i++) {
    RiskAccumulator& a = acc[i];
    riskAccumulateLive(a, out);
    if (haveRec && lastRec.ts >= now - RISK_WINDOW_SECONDS[i])
      a.rain += rainTips * cfg.rain_mm_tip;
    RiskWindow& w = out.windows[i];
    w.rain = a.rain;
    if (a.tempCount) w.temp = a.temp / a.tempCount;
    if (a.pressureCount) w.pressure = a.pressure / a.pressureCount;
    if (a.windCount) w.windAvg = a.wind / a.windCount;
    w.windGust = a.gust;
    if (a.humidityCount) w.humidity = a.humidity / a.humidityCount;
    if (!isnan(a.firstTemp) && !isnan(a.lastTemp)) w.tempChange = a.lastTemp - a.firstTemp;
    if (!isnan(a.firstPressure) && !isnan(a.lastPressure)) w.pressureChange = a.lastPressure - a.firstPressure;
  }
  return true;
}

static void riskJsonNumber(JsonObject o, const char* key, float value, uint8_t decimals = 2) {
  if (isnan(value) || !isfinite(value)) o[key] = nullptr;
  else o[key] = roundf(value * powf(10, decimals)) / powf(10, decimals);
}

static void riskWriteWindow(JsonObject o, const RiskWindow& w) {
  riskJsonNumber(o, "rain_mm", w.rain, 3);
  riskJsonNumber(o, "temp_avg_c", w.temp);
  riskJsonNumber(o, "pressure_avg_hpa", w.pressure);
  riskJsonNumber(o, "wind_avg_ms", w.windAvg);
  riskJsonNumber(o, "wind_gust_ms", w.windGust);
  riskJsonNumber(o, "humidity_avg_pct", w.humidity);
  riskJsonNumber(o, "temp_change_c", w.tempChange);
  riskJsonNumber(o, "pressure_change_hpa", w.pressureChange);
}

static String riskEncodeEvent(const RiskSnapshot& s, const String& tag, const String& notes) {
  JsonDocument d;
  d["ts"] = s.ts;
  d["tag"] = tag;
  d["notes"] = notes;
  JsonObject instant = d["instant"].to<JsonObject>();
  riskJsonNumber(instant, "temp_c", s.temp);
  riskJsonNumber(instant, "pressure_hpa", s.pressure);
  riskJsonNumber(instant, "humidity_pct", s.humidity);
  riskJsonNumber(instant, "wind_ms", s.wind);
  riskJsonNumber(instant, "wind_direction_deg", s.direction, 0);
  riskJsonNumber(instant, "battery_v", s.battery, 3);
  JsonArray windows = d["windows"].to<JsonArray>();
  for (uint8_t i = 0; i < RISK_WINDOWS; i++) {
    JsonObject window = windows.add<JsonObject>();
    window["hours"] = (i == 0) ? 3 : (i == 1) ? 6 : 12;
    riskWriteWindow(window, s.windows[i]);
  }
  String encoded;
  serializeJson(d, encoded);
  return encoded;
}

static bool riskAppendEvent(const String& encoded) {
  if (!sdOk) return false;
  SdLock lock;
  if (!lock.ok) return false;
  File f = SD.open(RISK_FILE, FILE_APPEND);
  if (!f) return false;
  size_t written = f.println(encoded);
  f.flush();
  f.close();
  return written == encoded.length() + 2;
}

// Removes the first report with this time and hazard type by rewriting the file without it.
// Returns 1 deleted, 0 not found, -1 SD error.
static int riskDeleteEvent(uint32_t ts, const String& tag) {
  if (!sdOk) return -1;
  SdLock lock;
  if (!lock.ok) return -1;
  File in = SD.open(RISK_FILE, FILE_READ);
  if (!in) return 0;
  File out = SD.open(RISK_TMP, FILE_WRITE);
  if (!out) { in.close(); return -1; }
  bool found = false, ok = true;
  char line[RISK_LINE_SIZE];
  while (in.available()) {
    int n = in.readBytesUntil('\n', line, sizeof(line) - 1);
    while (n && line[n - 1] == '\r') n--;
    line[n] = 0;
    if (!n) continue;
    if (!found) {
      JsonDocument e;
      if (!deserializeJson(e, line) && (uint32_t)(e["ts"] | 0UL) == ts && tag == (e["tag"] | "")) { found = true; continue; }
    }
    if (out.println(line) != (size_t)n + 2) { ok = false; break; }
  }
  in.close();
  out.close();
  if (!ok || !found) { SD.remove(RISK_TMP); return ok ? 0 : -1; }
  if (!SD.remove(RISK_FILE) || !SD.rename(RISK_TMP, RISK_FILE)) return -1;
  return 1;
}

static float riskProgress(float current, float event, float minimum, bool negative = false) {
  if (isnan(current) || isnan(event)) return NAN;
  if ((!negative && event < minimum) || (negative && event > -minimum)) return NAN;
  if (negative && current >= 0) return 0;
  return constrain(current / event, 0.0f, 1.0f);
}

static void riskConsiderEvent(JsonDocument& event, const RiskSnapshot& current, RiskAlert& best) {
  JsonArrayConst eventWindows = event["windows"].as<JsonArrayConst>();
  if (eventWindows.size() != RISK_WINDOWS) return;
  const float weights[] = {3.0f, 1.4f, 1.0f, 1.5f};
  const char* fields[] = {"rain_mm", "wind_gust_ms", "humidity_avg_pct", "pressure_change_hpa"};
  const float minima[] = {0.5f, 4.0f, 70.0f, 0.8f};
  // Only compare factors present in the logged event; accumulated rain outweighs a gust.
  for (uint8_t i = 0; i < RISK_WINDOWS; i++) {
    JsonObjectConst target = eventWindows[i];
    RiskWindow const& observed = current.windows[i];
    const float values[] = {observed.rain, observed.windGust, observed.humidity, observed.pressureChange};
    float weighted = 0, totalWeight = 0;
    uint8_t factors = 0;
    for (uint8_t f = 0; f < 4; f++) {
      float eventValue = target[fields[f]] | NAN;
      float progress = riskProgress(values[f], eventValue, minima[f], f == 3);
      if (isnan(progress)) continue;
      weighted += progress * weights[f];
      totalWeight += weights[f];
      factors++;
    }
    if (factors < 2 || totalWeight == 0) continue;
    float score = weighted / totalWeight;
    if (score <= best.score) continue;
    best.score = score;
    best.tag = event["tag"] | "hazard";
    best.windowHours = (i == 0) ? 3 : (i == 1) ? 6 : 12;
    best.level = score >= 0.88f ? "Critical" : score >= 0.72f ? "Warning" : score >= 0.55f ? "Advisory" : "none";
  }
}

static bool riskEvaluate(RiskAlert& alert) {
  alert = RiskAlert();
  if (!sdOk) return false;
  RiskSnapshot current;
  if (!riskCapture(clockNow(), current)) return false;
  SdLock lock;
  if (!lock.ok) return false;
  File f = SD.open(RISK_FILE, FILE_READ);
  if (!f) return true;
  char line[RISK_LINE_SIZE];
  while (f.available()) {
    int n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = 0;
    JsonDocument event;
    if (deserializeJson(event, line)) continue;
    riskConsiderEvent(event, current, alert);
  }
  f.close();
  return true;
}

static bool riskCurrentAlert(RiskAlert& alert) {
  if (!riskCacheReady || millis() - riskCacheMs >= 15000) {
    if (!riskEvaluate(cachedRiskAlert)) return false;
    riskCacheMs = millis();
    riskCacheReady = true;
  }
  alert = cachedRiskAlert;
  return true;
}
