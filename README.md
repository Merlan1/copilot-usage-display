# Copilot Usage OLED (ESP32 + SSD1306)

## Disclaimer

Slopped together by an AI after a brief discussion about wanting to see Copilot usage on a tiny screen. Almost none of this was written by a human. It mostly works, which is frankly surprising.

## Overview

- Host runs a Python server that calls `gh api` and exposes `/copilot-usage` JSON.
- Communication uses **WebSocket** (bidirectional): the server pushes updated data as soon as it fetches new GitHub Copilot usage.
- An HTTP endpoint (`/copilot-usage`) remains available for compatibility.
- ESP32 receives data in real time and shows usage on an SSD1306 OLED.
- A **physical button** (GPIO 0 / BOOT) triggers an immediate refresh.
- Per-model breakdown is shown as a **smooth-scrolling info line** on the display.

## Requirements

- Windows PC with GitHub CLI (`gh`) installed and authenticated.
- Python 3.8+.
- ESP32 board.
- SSD1306 OLED (128x32 recommended).
- Arduino IDE with libraries:
  - Adafruit SSD1306
  - Adafruit GFX
  - ArduinoJson (v7)
  - WebSockets (by Markus Sattler / Links2004)

## Host Setup (Windows)

1) Ensure `gh` is logged in:
```powershell
gh auth status
```

2) Install the required Python package:
```powershell
pip install websockets>=12.0
```

3) Start the server:
```powershell
python host\server.py
```

The server starts two endpoints:
- **HTTP** `http://<host-ip>:8732/copilot-usage` (for browsers / curl)
- **WebSocket** `ws://<host-ip>:8733` (for the ESP32)

Optional debug logging:
```powershell
$env:COPILOT_USAGE_DEBUG="1"
python host\server.py
```

## ESP32 Firmware Setup

1) Open `firmware/CopilotUsageDisplay/CopilotUsageDisplay.ino`.

2) Configure Wi-Fi credentials in `firmware/CopilotUsageDisplay/secrets.h`:
```cpp
#define SECRET_SSID "YOUR_WIFI_SSID"
#define SECRET_PASSWORD "YOUR_WIFI_PASSWORD"
```

3) Set the WebSocket host in `firmware/CopilotUsageDisplay/CopilotUsageDisplay.ino`:
```cpp
static const char *WS_HOST = "<host-ip>";
static const uint16_t WS_PORT = 8733;
```

4) (Optional) Set I2C pins if your board needs them:
```cpp
static const int I2C_SDA = 21;
static const int I2C_SCL = 22;
```

5) Upload to the ESP32.

## Display Layout

```
Copilot Usage               188/300
[████████████████░░░░░░]   37.3%
M:GPT-5.2-Codex 57.2%
```

- **Top row**: title (left) + remaining/limit quota (right).
- **Middle row**: progress bar + usage percentage.
- **Bottom row**: cycles through `avg/day` and per‑model percentages every 10 seconds with a smooth vertical slide animation.

## Button

Press the **BOOT button** (GPIO 0) on the ESP32 to send a `refresh` command to the host. The host immediately fetches fresh Copilot usage data from the GitHub API and pushes the update to all connected displays.

## Notes

- The display is configured for 128x32. If you have 128x64, update `SCREEN_HEIGHT`.
- Data is pushed automatically when the server refreshes (every 15 minutes by default). No polling needed.
- If the WebSocket disconnects, the ESP32 reconnects automatically every 5 seconds.
- If you see resets with `Brownout detector was triggered`, use a stronger power supply and a short USB cable.
- Keep real Wi-Fi credentials only in your local `secrets.h`; do not commit secrets.

## Files

- `host/server.py` - Python server (HTTP + WebSocket) using `gh api`.
- `host/requirements.txt` - Python dependencies.
- `firmware/CopilotUsageDisplay/CopilotUsageDisplay.ino` - ESP32 firmware.
