# Copilot Usage OLED (ESP32 + SSD1306)

This project was created with the help of AI assistance. Some code and documentation were generated or modified with AI involvement.

## Overview

- Host runs a Python HTTP server that calls `gh api` and exposes `/copilot-usage` JSON.
- ESP32 polls the endpoint and shows usage on an SSD1306 OLED.

## Requirements

- Windows PC with GitHub CLI (`gh`) installed and authenticated.
- Python 3.8+.
- ESP32 board.
- SSD1306 OLED (128x32 recommended based on the working example).
- Arduino IDE with libraries:
  - Adafruit SSD1306
  - Adafruit GFX
  - ArduinoJson (v6)

## Host Setup (Windows)

1) Ensure `gh` is logged in:
```powershell
gh auth status
```

2) Start the HTTP server:
```powershell
python host\server.py
```

3) Verify output in a browser:
```
http://<host-ip>:8732/copilot-usage
```

Optional debug logging:
```powershell
$env:COPILOT_USAGE_DEBUG="1"
python host\server.py
```

## ESP32 Firmware Setup

1) Open `firmware/CopilotUsageDisplay/CopilotUsageDisplay.ino`.

2) Set Wi-Fi and endpoint:
```cpp
static const char *WIFI_SSID = "YOUR_WIFI_SSID";
static const char *WIFI_PASS = "YOUR_WIFI_PASSWORD";
static const char *USAGE_URL = "http://<host-ip>:8732/copilot-usage";
```

3) (Optional) Set I2C pins if your board needs them:
```cpp
static const int I2C_SDA = 21;
static const int I2C_SCL = 22;
```

4) Upload to the ESP32.

## Notes

- The display is configured for 128x32. If you have 128x64, update `SCREEN_HEIGHT`.
- The device polls every 10 minutes (`POLL_INTERVAL_MS`).
- If you see resets with `Brownout detector was triggered`, use a stronger power supply and a short USB cable.

## Files

- `host/server.py` - Python HTTP server using `gh api`.
- `firmware/CopilotUsageDisplay/CopilotUsageDisplay.ino` - ESP32 firmware.
