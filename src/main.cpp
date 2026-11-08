// ESP32 weather station (USB-C powered; solar battery optional)
//
// Tasks:
//   controllerTask  - owns I2C/SD writes, sampling cycle, light sleep, button, upload scheduling
//   async_tcp       - ESPAsyncWebServer's own task; only runs while a session is active
//   (WiFi/lwIP tasks are started by the SDK)
// Light sleep halts every task, so ONE task decides when to sleep and the web server is shut
// down first. Wake sources: DS3231 alarm (GPIO33), button (GPIO32), rain Hall (GPIO14).
#include <Arduino.h>
#include <LittleFS.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <esp_system.h>

#include "state.h"
#include "storage.h"
#include "sensors.h"
#include "auth.h"
#include "session.h"
#include "upload.h"
#include "web.h"

// ---------------------------------------------------------------- scheduling
static void scheduleNext() {
  uint32_t t = clockNow();
  uint32_t iv = cfg.interval_s;
  nextSample = (t / iv + 1) * iv;               // aligned to wall-clock multiples
  if (nextSample - t < 3) nextSample += iv;
  if (rtcOk) {
    rtc.setAlarm1(DateTime(nextSample), DS3231_A1_Hour);   // match hh:mm:ss
    rtc.clearAlarm(1);
  }
}

static char lastDay[12] = "";
static void dayRollover(uint32_t now) {
  char d[12];
  fmtDate(now, d);
  if (strcmp(d, lastDay) != 0) {
    if (lastDay[0]) rainTodayMm = 0;
    strcpy(lastDay, d);
  }
}

static void doSample() {
  Rec r;
  sampleCycle(r, sessionActive);
  r.ts = clockNow();
  if (!sdLog(r)) Serial.println("SD log failed");
  lastRec = r;
  haveRec = true;
  scheduleNext();
}

// ---------------------------------------------------------------- admin reset
static void attemptAdminReset() {
  uint32_t now = clockNow();
  Preferences p;
  p.begin("state", false);
  uint32_t last = p.getUInt("rst", 0);
  if (last && now > last && now - last < RESET_MIN_INTERVAL_S) {
    uint32_t until = last + RESET_MIN_INTERVAL_S;
    char d[12];
    fmtDate(until, d);
    resetMsg = String("Reset refused: allowed once per 30 days. Next possible on ") + d + ".";
    p.end();
    return;
  }
  p.putUInt("rst", now);
  p.end();
  authClearAll();
  setupOpenUntil = millis() + SETUP_WINDOW_MS;
  resetMsg = "Admin password cleared. Set a new one within 15 minutes.";
}

// ---------------------------------------------------------------- button
static void handleButton() {
  delay(30);
  if (digitalRead(PIN_BUTTON) != LOW) return;       // bounce
  uint32_t t0 = millis();
  bool resetDone = false;
  while (digitalRead(PIN_BUTTON) == LOW && millis() - t0 < 60000) {
    rainService();
    delay(20);
    if (!resetDone && millis() - t0 >= RESET_HOLD_MS) {
      timeRefresh();
      attemptAdminReset();
      resetDone = true;
    }
  }
  delay(30);
  if (!resetDone) {
    float b = readSupply();
    if (cfg.power_mode == POWER_BATTERY && b < cfg.batt_min_wifi && !sessionActive) {
      resetMsg = "Battery too low for Wi-Fi (" + String(b, 2) + " V).";
      return;
    }
    resetMsg = "";
  }
  sessionStart();
}

static void pollEvents() {
  rainService();
  if (digitalRead(PIN_RTC_INT) == LOW && rtcOk) rtc.clearAlarm(1);   // release INT line
  if (digitalRead(PIN_BUTTON) == LOW) handleButton();
}

// ---------------------------------------------------------------- light sleep
static void lightSleep() {
  uint32_t now = clockNow();
  uint32_t target = nextSample;
  if (cfg.up_en && cfg.up_url.length() && nextUpload > now && nextUpload < target) target = nextUpload;
  int32_t secs = (int32_t)(target - now);
  if (secs < 1) return;
  if (secs > 3600) secs = 3600;
  if (digitalRead(PIN_RTC_INT) == LOW || digitalRead(PIN_BUTTON) == LOW) return;  // would wake instantly

  rainService();
  esp_sleep_enable_timer_wakeup((uint64_t)(secs + 3) * 1000000ULL);   // backup if RTC alarm is missed
  gpio_wakeup_enable((gpio_num_t)PIN_RTC_INT, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)PIN_BUTTON, GPIO_INTR_LOW_LEVEL);
  // Rain: wake on the *opposite* of the current level so a resting magnet can't spin us awake.
  gpio_wakeup_enable((gpio_num_t)PIN_RAIN, rainLast ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  Serial.flush();
  esp_light_sleep_start();
  gpio_wakeup_disable((gpio_num_t)PIN_RTC_INT);
  gpio_wakeup_disable((gpio_num_t)PIN_BUTTON);
  gpio_wakeup_disable((gpio_num_t)PIN_RAIN);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
}

// ---------------------------------------------------------------- controller task
static void controllerTask(void*) {
  timeRefresh();
  dayRollover(clockNow());
  scheduleNext();
  doSample();                                       // first record right after boot
  for (;;) {
    timeRefresh();
    uint32_t now = clockNow();
    dayRollover(now);
    pollEvents();
    if (now >= nextSample) doSample();
    uploadTick(clockNow());
    if (sessionActive) {
      ntpTick();
      liveStep();
      if (endRequested || sessionLeftS() == 0) {
        endRequested = false;
        sessionEnd();
      }
    } else {
      lightSleep();
    }
  }
}

// ---------------------------------------------------------------- power-up log
// USB power has no battery behind it, so every power cut or brownout restarts the station.
// Each start is appended to /power.log on the SD card and shown on the Live page.
static const char* resetReasonText(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "power on";        // power restored/plugged in, or EN pressed
    case ESP_RST_BROWNOUT: return "brownout";        // supply dipped: weak adapter, long or thin cable
    case ESP_RST_SW:       return "software restart";
    case ESP_RST_PANIC:    return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "watchdog";
    default:               return "other";
  }
}

static void logPowerUp() {
  bootReason = resetReasonText(esp_reset_reason());
  bootEpoch = clockNow();
  float v = readSupply();
  Serial.printf("Start: %s, supply %.2f V\n", bootReason, isnan(v) ? 0.0f : v);
  if (!sdOk) return;
  char iso[32];
  fmtLocalIso(bootEpoch, iso, sizeof iso);
  SdLock l;
  if (!l.ok) return;
  bool fresh = !SD.exists("/power.log");
  File f = SD.open("/power.log", FILE_APPEND);
  if (!f) return;
  if (fresh) f.println("local_time,reason,supply_v,power_mode");
  f.printf("%s,%s,%s,%s\n", iso, bootReason, isnan(v) ? "" : String(v, 2).c_str(),
           cfg.power_mode == POWER_BATTERY ? "battery" : "usb");
  f.close();
}

// ---------------------------------------------------------------- setup
static void rtcInit() {
  rtcOk = rtc.begin();
  if (!rtcOk) { Serial.println("DS3231 not found"); return; }
  rtc.writeSqwPinMode(DS3231_OFF);      // SQW pin becomes the alarm interrupt output
  rtc.disable32K();
  rtc.disableAlarm(2);
  rtc.clearAlarm(1);
  rtc.clearAlarm(2);
  if (rtc.lostPower()) {
    // Fall back to the build time (assumed local) until the admin or NTP sets the clock.
    DateTime c(F(__DATE__), F(__TIME__));
    rtc.adjust(DateTime(c.unixtime() - (int32_t)cfg.tz_min * 60));
    timeTrusted = false;
  } else {
    timeTrusted = true;
  }
}

void setup() {
  Serial.begin(115200);
  setCpuFrequencyMhz(80);
  btStop();
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);

  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_RTC_INT, INPUT_PULLUP);

  sdMutex = xSemaphoreCreateMutex();
  cfgLoad();
  LittleFS.begin(true);
  // Wi-Fi + dashboard come up first, as before. Missing modules (RTC, SD, barometer) never
  // stop startup; they are shown as warnings on the dashboard instead.
  Serial.println("boot: sensors"); sensorsInit();
  Serial.println("boot: wifi/web"); webInit(); sessionStart();
  Serial.println("boot: rtc"); rtcInit();
  timeRefresh();
  Serial.println("boot: sd"); sdInit();
  if (sdOk && !sdRecoverCsvSchema()) Serial.println("SD CSV schema recovery failed");
  sdSeedToday();
  logPowerUp();
  syncLoad();
  nextUpload = 0;
  Serial.println("boot: done");

  xTaskCreatePinnedToCore(controllerTask, "ctrl", 16384, nullptr, 3, nullptr, 1);
}

void loop() { vTaskDelay(portMAX_DELAY); }
