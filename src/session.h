#pragma once
// "Awake session": Wi-Fi AP(+STA), mDNS and the web server are only up while someone
// is using the station (button press) or while an upload is running.
#include <WiFi.h>
#include <ESPmDNS.h>
#include <esp_sntp.h>
#include <ESPAsyncWebServer.h>
#include "state.h"
#include "sensors.h"

static AsyncWebServer server(80);
static bool ntpStarted = false;

static uint32_t sessionLeftS() {
  if (!sessionActive) return 0;
  uint32_t el = millis() - lastActivityMs;
  uint32_t tot = (uint32_t)cfg.session_s * 1000UL;
  return el >= tot ? 0 : (tot - el) / 1000;
}

static bool staHasCreds(String* ssid = nullptr, String* pass = nullptr) {
  Preferences p;
  p.begin("net", false);
  String s = p.getString("ssid", ""), w = p.getString("pass", "");
  p.end();
  if (ssid) *ssid = s;
  if (pass) *pass = w;
  return s.length() > 0;
}

static void staBegin() {
  String s, w;
  if (staHasCreds(&s, &w)) WiFi.begin(s.c_str(), w.c_str());
}

static bool staConnected() { return WiFi.status() == WL_CONNECTED; }

// Non-blocking: call repeatedly. Sets the RTC from NTP when a router is reachable.
static bool ntpTick() {
  if (!staConnected()) return false;
  if (!ntpStarted) {
    sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    ntpStarted = true;
    return false;
  }
  if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
    applyEpoch((uint32_t)time(nullptr));
    return true;
  }
  return false;
}

static void sessionStart() {
  if (sessionActive) { touch(); return; }
  WiFi.mode(WIFI_AP_STA);
  WiFi.setHostname(cfg.host.c_str());
  String ap = cfg.ap_pass.length() >= 8 ? cfg.ap_pass : String(DEFAULT_AP_PASS);
  WiFi.softAP(cfg.ap_ssid.c_str(), ap.c_str());
  staBegin();
  if (MDNS.begin(cfg.host.c_str())) MDNS.addService("http", "tcp", 80);
  server.begin();
  sensorsPower(true);
  pcntTake();
  ntpStarted = false;
  sessionActive = true;
  touch();
}

static void sessionEnd() {
  server.end();
  MDNS.end();
  WiFi.disconnect(true);
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  sensorsPower(false);
  sessionActive = false;
  ntpStarted = false;
}
