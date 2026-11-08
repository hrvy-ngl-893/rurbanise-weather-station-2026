#pragma once
#include <Arduino.h>

// ---------------- Pin map ----------------
#define PIN_I2C_SDA     21   // BMP180 + DS3231
#define PIN_I2C_SCL     22
#define PIN_SD_SCK      18
#define PIN_SD_MISO     19
#define PIN_SD_MOSI     23
#define PIN_SD_CS       5
#define PIN_RTC_INT     33   // DS3231 SQW/INT, active low, 10k pull-up
#define PIN_BUTTON      32   // admin/wake button, active low
#define PIN_RAIN        14   // tipping-bucket Hall sensor (always powered)
#define PIN_ANEMO       27   // anemometer Hall sensor -> PCNT
#define PIN_VANE_N      25   // wind vane Hall sensors (digital)
#define PIN_VANE_E      26
#define PIN_VANE_S      13
#define PIN_VANE_W      4
#define PIN_DHT22       16   // DHT22 data (GPIO16 is available on ESP32-WROOM boards)
#define PIN_VSENSE      34   // ADC1: supply sense. USB mode: ESP32 5V/VIN pin via 200k/100k divider.
                             // Solar mode: the 18650 at the TP4056 OUT+ via the same divider.

// Optional switched rail for anemometer + vane sensors. Without a gate,
// power these sensors and their pull-ups from 3.3 V; they remain on during sleep.
#define PIN_SENSOR_PWR  -1
#define SENSOR_PWR_ON   HIGH

#define VANE_ACTIVE_LEVEL LOW      // level a Hall sensor outputs when the magnet is over it
#define VSENSE_DIVIDER    3.0f     // (200k + 100k) / 100k: two 100k in series on top, one 100k to GND

// ---------------- Power source ----------------
// The station is powered through the ESP32's USB-C port. Admin > "Power source" selects what GPIO34 measures:
//   POWER_USB     wall adapter / power bank / mini UPS; the ADC reads the 5 V rail. No Wi-Fi or upload gating.
//   POWER_BATTERY future solar kit (one 18650 in the box); the ADC reads the cell. batt_min_wifi / batt_min_upload apply.
#define POWER_USB         0
#define POWER_BATTERY     1
#define USB_LOW_V         4.40f    // dashboard warns below this in USB mode: the 3.3 V regulator starts to drop out
#define VSENSE_MIN_V      0.50f    // below this the divider is treated as not fitted (reading shown as "—")

// ---------------- Behaviour ----------------
#define RESET_HOLD_MS          10000UL
#define RESET_MIN_INTERVAL_S   (30UL * 86400UL)
#define SETUP_WINDOW_MS        (15UL * 60UL * 1000UL)
#define SESSION_TOKEN_TTL_MS   (30UL * 60UL * 1000UL)
#define PBKDF2_ITER            8000
#define RAIN_MIN_TIP_GAP_MS    250
#define UPLOAD_BATCH_BYTES     6000
#define UPLOAD_MAX_BATCHES     30
#define DEFAULT_AP_PASS        "weather1234"

#define CSV_HEADER_V1 "epoch_utc,local_iso,temp_c,pressure_hpa,wind_avg_ms,wind_gust_ms,wind_dir_deg,rain_mm,batt_v"
#define CSV_HEADER "epoch_utc,local_iso,temp_c,pressure_hpa,wind_avg_ms,wind_gust_ms,wind_dir_deg,rain_mm,batt_v,humidity_pct"
