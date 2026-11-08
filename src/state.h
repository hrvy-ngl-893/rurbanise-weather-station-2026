#pragma once
// Shared configuration, globals and time helpers.
// The firmware is built as ONE translation unit (main.cpp includes every module),
// so `static` globals here are shared by all modules.
#include <Arduino.h>
#include <Preferences.h>
#include <RTClib.h>
#include <sys/time.h>
#include <time.h>
#include <math.h>
#include "config.h"

// ---------------------------------------------------------------- config
struct Config {
  uint32_t interval_s = 300;       // log interval
  uint16_t window_s = 10;          // wind measurement window per cycle
  float wind_k = 0.667f;           // m/s per Hz of anemometer pulses (CALIBRATE)
  float rain_mm_tip = 0.2f;        // mm per bucket tip: v2 gauge (rain_gauge.py) = 3.00 mL / 138.3 mm funnel (CALIBRATE)
  float altitude_m = 0;            // 0 = log station pressure
  int16_t tz_min = 480;            // Asia/Manila, no DST
  int16_t vane_off = 0;            // vane mounting offset, degrees
  uint16_t session_s = 600;        // admin/AP session idle timeout
  uint8_t power_mode = POWER_USB;  // POWER_USB or POWER_BATTERY (solar upgrade)
  float batt_min_wifi = 3.40f;     // battery mode only
  float batt_min_upload = 3.55f;   // battery mode only
  String ap_ssid = "WeatherStation";
  String ap_pass = DEFAULT_AP_PASS;
  String host = "weatherstation";
  bool up_en = false;
  String up_url;
  String up_tok;
  uint16_t up_int_min = 60;
  bool up_insecure = false;
  double lat = NAN;                // station location (decimal degrees, WGS84); NAN = not set
  double lon = NAN;
};
static Config cfg;

static void cfgLoad() {
  Preferences p;
  p.begin("cfg", false);
  cfg.interval_s = p.getUInt("iv", cfg.interval_s);
  cfg.window_s = p.getUShort("win", cfg.window_s);
  cfg.wind_k = p.getFloat("wk", cfg.wind_k);
  cfg.rain_mm_tip = p.getFloat("rmm", cfg.rain_mm_tip);
  cfg.altitude_m = p.getFloat("alt", cfg.altitude_m);
  cfg.tz_min = p.getShort("tz", cfg.tz_min);
  cfg.vane_off = p.getShort("voff", cfg.vane_off);
  cfg.session_s = p.getUShort("sess", cfg.session_s);
  cfg.power_mode = p.getUChar("pmode", cfg.power_mode);
  cfg.batt_min_wifi = p.getFloat("bwifi", cfg.batt_min_wifi);
  cfg.batt_min_upload = p.getFloat("bup", cfg.batt_min_upload);
  cfg.ap_ssid = p.getString("apssid", cfg.ap_ssid);
  cfg.ap_pass = p.getString("appass", cfg.ap_pass);
  cfg.host = p.getString("host", cfg.host);
  cfg.up_en = p.getBool("upen", false);
  cfg.up_url = p.getString("upurl", "");
  cfg.up_tok = p.getString("uptok", "");
  cfg.up_int_min = p.getUShort("upint", cfg.up_int_min);
  cfg.up_insecure = p.getBool("upins", false);
  cfg.lat = p.getDouble("lat", NAN);
  cfg.lon = p.getDouble("lon", NAN);
  p.end();
  if (cfg.interval_s < 60) cfg.interval_s = 60;
  if (cfg.window_s < 3) cfg.window_s = 3;
  if (cfg.window_s > 60) cfg.window_s = 60;
  if (cfg.power_mode > POWER_BATTERY) cfg.power_mode = POWER_USB;
}

static void cfgSave() {
  Preferences p;
  p.begin("cfg", false);
  p.putUInt("iv", cfg.interval_s);
  p.putUShort("win", cfg.window_s);
  p.putFloat("wk", cfg.wind_k);
  p.putFloat("rmm", cfg.rain_mm_tip);
  p.putFloat("alt", cfg.altitude_m);
  p.putShort("tz", cfg.tz_min);
  p.putShort("voff", cfg.vane_off);
  p.putUShort("sess", cfg.session_s);
  p.putUChar("pmode", cfg.power_mode);
  p.putFloat("bwifi", cfg.batt_min_wifi);
  p.putFloat("bup", cfg.batt_min_upload);
  p.putString("apssid", cfg.ap_ssid);
  p.putString("appass", cfg.ap_pass);
  p.putString("host", cfg.host);
  p.putBool("upen", cfg.up_en);
  p.putString("upurl", cfg.up_url);
  p.putString("uptok", cfg.up_tok);
  p.putUShort("upint", cfg.up_int_min);
  p.putBool("upins", cfg.up_insecure);
  if (isnan(cfg.lat) || isnan(cfg.lon)) { p.remove("lat"); p.remove("lon"); }
  else { p.putDouble("lat", cfg.lat); p.putDouble("lon", cfg.lon); }
  p.end();
}

// ---------------------------------------------------------------- records
struct Rec {
  uint32_t ts = 0;
  float temp = NAN, pres = NAN, humidity = NAN, wavg = NAN, wgust = NAN;
  int16_t wdir = -1;
  float rain = NAN, batt = NAN;    // batt = GPIO34 volts: USB 5 V rail, or the battery in solar mode
};

// ---------------------------------------------------------------- globals
static SemaphoreHandle_t sdMutex;
static RTC_DS3231 rtc;
static bool sdOk = false, bmpOk = false, rtcOk = false, timeTrusted = false;

static Rec lastRec;
static bool haveRec = false;
static float liveWind = 0, liveTemp = NAN, livePres = NAN, liveHumidity = NAN, liveBatt = NAN;
static int liveDir = -1;
static float rainTodayMm = 0;

static volatile bool sessionActive = false;
static volatile bool endRequested = false;
static volatile uint32_t lastActivityMs = 0;
static volatile uint32_t pendingEpoch = 0;     // set by web, applied by controller
static volatile bool rescheduleReq = false;    // set by web, handled by controller
static String resetMsg;
static uint32_t bootEpoch = 0;                 // when this power-up started (for power-cut tracking)
static const char* bootReason = "";

static String uploadStatus = "idle";
static volatile bool uploadTestReq = false, uploadNowReq = false;
static String syncFileInfo;
static uint32_t syncOffInfo = 0;

static uint32_t nextSample = 0, nextUpload = 0;
static void scheduleNext();                    // defined in main.cpp

// ---------------------------------------------------------------- time
// Only the controller task touches the RTC (I2C). Everyone else reads the cache.
static uint32_t epochBase = 0, epochBaseMs = 0;

static uint32_t clockNow() { return epochBase + (millis() - epochBaseMs) / 1000; }

static void applyEpoch(uint32_t t) {
  epochBase = t;
  epochBaseMs = millis();
  if (rtcOk) rtc.adjust(DateTime(t));
  struct timeval tv = {(time_t)t, 0};
  settimeofday(&tv, nullptr);
  timeTrusted = true;
  scheduleNext();
}

static void timeRefresh() {
  if (rescheduleReq) { rescheduleReq = false; scheduleNext(); }
  if (pendingEpoch) {
    uint32_t t = pendingEpoch;
    pendingEpoch = 0;
    applyEpoch(t);
  }
  epochBase = rtcOk ? rtc.now().unixtime() : (uint32_t)time(nullptr);
  epochBaseMs = millis();
  // Keep the system clock (used by TLS certificate checks) in step with the RTC.
  if (rtcOk && labs((long)time(nullptr) - (long)epochBase) > 2 && epochBase > 1735689600UL) {
    struct timeval tv = {(time_t)epochBase, 0};
    settimeofday(&tv, nullptr);
  }
}

static void localTm(uint32_t ts, struct tm* out) {
  time_t t = (time_t)ts + (time_t)cfg.tz_min * 60;
  gmtime_r(&t, out);
}

static void fmtDate(uint32_t ts, char* out /*>=11*/) {
  struct tm tm;
  localTm(ts, &tm);
  strftime(out, 11, "%Y-%m-%d", &tm);
}

static void fileForEpoch(uint32_t ts, char* out /*>=16*/) {
  char d[12];
  fmtDate(ts, d);
  snprintf(out, 16, "/%s.csv", d);
}

static void fmtLocalIso(uint32_t ts, char* out, size_t n) {
  struct tm tm;
  localTm(ts, &tm);
  char b[24];
  strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%S", &tm);
  int a = abs(cfg.tz_min);
  snprintf(out, n, "%s%c%02d:%02d", b, cfg.tz_min < 0 ? '-' : '+', a / 60, a % 60);
}

static void touch() { lastActivityMs = millis(); }

static bool locationSet() { return !isnan(cfg.lat) && !isnan(cfg.lon); }
// ",lat,lon" for the end of a CSV row (empty fields when no location is set); downloads and uploads add it.
static String locSuffix() {
  return locationSet() ? "," + String(cfg.lat, 6) + "," + String(cfg.lon, 6) : String(",,");
}
