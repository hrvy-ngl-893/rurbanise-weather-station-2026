#pragma once
// Sensors: BMP180 (I2C), anemometer (PCNT), 4-Hall wind vane, tipping-bucket rain, supply voltage.
#include <Wire.h>
#include <Adafruit_BMP085.h>
#include <DHT.h>
#include <driver/pcnt.h>
#include "state.h"

static Adafruit_BMP085 bmp;   // BMP180 is driven by the BMP085 library
static DHT dht(PIN_DHT22, DHT22);
static const char* bmpError = nullptr;
static bool bmpInitErrorLogged = false;
static bool bmpRangeErrorLogged = false;

static bool bmpReadChipId(uint8_t& id) {
  Wire.beginTransmission(0x77);
  Wire.write(0xD0);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)0x77, (uint8_t)1) != 1) return false;
  id = Wire.read();
  return true;
}

#if PIN_SENSOR_PWR >= 0
  #define SW_PIN_MODE INPUT          // pull-ups live on the switched rail
#else
  #define SW_PIN_MODE INPUT_PULLUP
#endif

// ---------------------------------------------------------------- power gate
static void sensorsPower(bool on) {
#if PIN_SENSOR_PWR >= 0
  digitalWrite(PIN_SENSOR_PWR, on ? SENSOR_PWR_ON : !SENSOR_PWR_ON);
#endif
}

// ---------------------------------------------------------------- rain
// Polled state machine (no ISR): robust against the magnet resting near the sensor,
// and the same function is used when waking from light sleep and while awake.
static bool rainLast = true;
static uint32_t rainTips = 0;          // tips since last logged record
static uint32_t lastTipMs = 0;

static void rainService() {
  bool lvl = digitalRead(PIN_RAIN);
  if (lvl == rainLast) return;
  if (!lvl) {                           // falling edge = tip
    delay(15);
    if (digitalRead(PIN_RAIN)) return;  // glitch
    if (millis() - lastTipMs >= RAIN_MIN_TIP_GAP_MS) {
      rainTips++;
      rainTodayMm += cfg.rain_mm_tip;
      lastTipMs = millis();
    }
  }
  rainLast = lvl;
}

// ---------------------------------------------------------------- anemometer
static void pcntInit() {
  pinMode(PIN_ANEMO, SW_PIN_MODE);
  pcnt_config_t c = {};
  c.pulse_gpio_num = PIN_ANEMO;
  c.ctrl_gpio_num = PCNT_PIN_NOT_USED;
  c.channel = PCNT_CHANNEL_0;
  c.unit = PCNT_UNIT_0;
  c.pos_mode = PCNT_COUNT_INC;
  c.neg_mode = PCNT_COUNT_DIS;
  c.lctrl_mode = PCNT_MODE_KEEP;
  c.hctrl_mode = PCNT_MODE_KEEP;
  c.counter_h_lim = 32767;
  c.counter_l_lim = -32768;
  pcnt_unit_config(&c);
  pcnt_set_filter_value(PCNT_UNIT_0, 1023);   // ~12.8 us glitch filter @80 MHz APB
  pcnt_filter_enable(PCNT_UNIT_0);
  pcnt_counter_pause(PCNT_UNIT_0);
  pcnt_counter_clear(PCNT_UNIT_0);
  pcnt_counter_resume(PCNT_UNIT_0);
}

static int pcntTake() {
  int16_t c = 0;
  pcnt_get_counter_value(PCNT_UNIT_0, &c);
  pcnt_counter_clear(PCNT_UNIT_0);
  return c;
}

// ---------------------------------------------------------------- vane
static uint8_t vaneMask() {
  uint8_t m = 0;
  if (digitalRead(PIN_VANE_N) == VANE_ACTIVE_LEVEL) m |= 1;
  if (digitalRead(PIN_VANE_E) == VANE_ACTIVE_LEVEL) m |= 2;
  if (digitalRead(PIN_VANE_S) == VANE_ACTIVE_LEVEL) m |= 4;
  if (digitalRead(PIN_VANE_W) == VANE_ACTIVE_LEVEL) m |= 8;
  return m;
}

// One sensor = cardinal, two adjacent = intercardinal. Anything else is invalid.
static int vaneDeg(uint8_t m) {
  int d;
  switch (m) {
    case 1:  d = 0;   break;   // N
    case 3:  d = 45;  break;   // N+E
    case 2:  d = 90;  break;   // E
    case 6:  d = 135; break;   // E+S
    case 4:  d = 180; break;   // S
    case 12: d = 225; break;   // S+W
    case 8:  d = 270; break;   // W
    case 9:  d = 315; break;   // W+N
    default: return -1;
  }
  return ((d + cfg.vane_off) % 360 + 360) % 360;
}

// ---------------------------------------------------------------- supply voltage
// USB 5 V rail (default) or the solar battery, depending on what the divider is wired to (cfg.power_mode).
static float readSupply() {
  uint32_t s = 0;
  for (int i = 0; i < 16; i++) { s += analogReadMilliVolts(PIN_VSENSE); delayMicroseconds(200); }
  float v = (s / 16.0f) * VSENSE_DIVIDER / 1000.0f;
  return v < VSENSE_MIN_V ? NAN : v;
}

// ---------------------------------------------------------------- init
static void sensorsInit() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(100000);
  pinMode(PIN_RAIN, INPUT_PULLUP);
  rainLast = digitalRead(PIN_RAIN);
#if PIN_SENSOR_PWR >= 0
  pinMode(PIN_SENSOR_PWR, OUTPUT);
#endif
  sensorsPower(false);
  pinMode(PIN_VANE_N, SW_PIN_MODE);
  pinMode(PIN_VANE_E, SW_PIN_MODE);
  pinMode(PIN_VANE_S, SW_PIN_MODE);
  pinMode(PIN_VANE_W, SW_PIN_MODE);
  pcntInit();
  analogSetPinAttenuation(PIN_VSENSE, ADC_11db);
  pinMode(PIN_VSENSE, INPUT);
  dht.begin();
  bmpOk = bmp.begin();
  if (!bmpOk) {
    uint8_t id = 0;
    if (!bmpReadChipId(id)) {
      bmpError = "No I2C response from barometer at 0x77";
    } else if (id != 0x55) {
      static char idError[64];
      snprintf(idError, sizeof(idError), "Unexpected barometer chip ID 0x%02X at 0x77 (BMP180 expects 0x55)", id);
      bmpError = idError;
    } else {
      bmpError = "BMP180 at 0x77 failed initialization; check I2C wiring and power";
    }
    Serial.println(bmpError);
    bmpInitErrorLogged = true;
  }
}

// ---------------------------------------------------------------- sampling
static void readEnvironment(float& t, float& p) {
  t = NAN; p = NAN;
  if (!bmpOk) {
    bmpOk = bmp.begin();
    if (!bmpOk) {
      uint8_t id = 0;
      if (!bmpReadChipId(id)) {
        bmpError = "No I2C response from barometer at 0x77";
      } else if (id != 0x55) {
        static char idError[64];
        snprintf(idError, sizeof(idError), "Unexpected barometer chip ID 0x%02X at 0x77 (BMP180 expects 0x55)", id);
        bmpError = idError;
      } else {
        bmpError = "BMP180 at 0x77 failed initialization; check I2C wiring and power";
      }
      if (!bmpInitErrorLogged) Serial.println(bmpError);
      bmpInitErrorLogged = true;
      return;
    }
    bmpInitErrorLogged = false;
  }
  float tt = bmp.readTemperature();
  float pp = (cfg.altitude_m != 0) ? bmp.readSealevelPressure(cfg.altitude_m) / 100.0f
                                    : bmp.readPressure() / 100.0f;
  if (!isfinite(tt) || !isfinite(pp) || tt < -40 || tt > 85 || pp < 300 || pp > 1100) {
    bmpOk = false;
    bmpError = "BMP180 returned readings outside the expected range";
    if (!bmpRangeErrorLogged) Serial.printf("BMP180 out-of-range reading: %.2f C, %.2f hPa\n", tt, pp);
    bmpRangeErrorLogged = true;
    return;
  }
  bmpError = nullptr;
  bmpRangeErrorLogged = false;
  t = tt; p = pp;
}

static float readHumidity() {
  float h = dht.readHumidity();
  return (isnan(h) || h < 0 || h > 100) ? NAN : h;
}

// Full measurement cycle. keepPowered = leave the sensor rail on (admin session).
static void sampleCycle(Rec& r, bool keepPowered) {
  r = Rec();
  readEnvironment(r.temp, r.pres);
  r.humidity = readHumidity();
  r.batt = readSupply();

  uint16_t n = cfg.window_s;
  float sp[60];
  double sx = 0, sy = 0;
  int valid = 0;

  sensorsPower(true);
  delay(40);
  pcntTake();
  for (uint16_t i = 0; i < n; i++) {
    uint32_t t0 = millis();
    while (millis() - t0 < 1000) { rainService(); delay(10); }
    sp[i] = pcntTake() * cfg.wind_k;            // pulses in ~1 s => Hz
    int d = vaneDeg(vaneMask());
    if (d >= 0) { sx += sin(d * DEG_TO_RAD); sy += cos(d * DEG_TO_RAD); valid++; }
    liveWind = sp[i];
    liveDir = d;
  }
  if (!keepPowered) sensorsPower(false);

  float sum = 0;
  for (uint16_t i = 0; i < n; i++) sum += sp[i];
  r.wavg = sum / n;

  // Gust = highest 3-second rolling mean (WMO convention), or max if window < 3.
  float g = 0;
  for (uint16_t i = 0; i + 2 < n; i++) g = max(g, (sp[i] + sp[i + 1] + sp[i + 2]) / 3.0f);
  r.wgust = (n >= 3) ? g : r.wavg;

  if (valid) {
    float deg = atan2(sx, sy) * RAD_TO_DEG;
    if (deg < 0) deg += 360;
    r.wdir = (int16_t)lroundf(deg) % 360;
  }

  r.rain = rainTips * cfg.rain_mm_tip;
  rainTips = 0;
  liveTemp = r.temp; livePres = r.pres; liveHumidity = r.humidity; liveBatt = r.batt;
}

// ~1 s step used while a session is active: refreshes the live readout.
static void liveStep() {
  static uint8_t k = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < 1000) { rainService(); delay(10); }
  liveWind = pcntTake() * cfg.wind_k;
  liveDir = vaneDeg(vaneMask());
  if (++k >= 10) {
    k = 0;
    readEnvironment(liveTemp, livePres);
    liveHumidity = readHumidity();
    liveBatt = readSupply();
  }
}
