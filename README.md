# ESP32 Weather Station firmware

Firmware and web app for the community weather station (ESP32 DevKit, PlatformIO).

## Flash with PlatformIO in VS Code

1. Install [VS Code](https://code.visualstudio.com/) and the **PlatformIO IDE** extension.
2. Clone the repository and open the folder in VS Code (**File > Open Folder**). The first time, PlatformIO downloads the toolchain and libraries; wait until that finishes.
3. Plug the ESP32 in with a USB **data** cable. If no port shows up, install the CP210x or CH340 USB driver (whichever chip your board has).
4. In the PlatformIO sidebar, under **esp32dev**:
   - **Upload**: flashes the firmware.
   - **Upload Filesystem Image**: flashes the web app (`data/www`). Do this after a first flash or whenever `data/` changes.
   - **Monitor**: serial log at 115200 baud.

   If the upload stalls at `Connecting...`, hold the board's **BOOT** button until it starts writing.

Flashing does not erase the station's settings (Wi-Fi, password, location) or the SD card data.

## Command line

```sh
pio run -t upload        # firmware
pio run -t uploadfs      # web app
pio device monitor       # serial log
```

With more than one board plugged in, add `--upload-port /dev/cu.usbserial-XXXX` (macOS) or `--upload-port COM5` (Windows).

## After flashing

Press the station button, join the Wi-Fi **WeatherStation** (password `weather1234`), and open <http://weatherstation.local> (or <http://192.168.4.1>).

## Layout

| Path | Contents |
|---|---|
| `src/` | Firmware: sensors, logging, web server, upload |
| `include/config.h` | Pin map and constants |
| `data/www/` | Web app, served from LittleFS |
