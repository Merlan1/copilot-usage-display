# Copilot Usage OLED (ESP32 + SSD1306)

## Disclaimer

Slopped together by an AI after a brief discussion about wanting to see Copilot usage on a tiny screen. Almost none of this was written by a human. It mostly works, which is frankly surprising.

## Overview
<img width="361" height="361" alt="grafik" src="https://github.com/user-attachments/assets/bdd1a67e-b035-4da8-8e9c-945dae4c86be" />


- Host runs a Python server that calls `gh api` and exposes `/copilot-usage` JSON.
- Communication to the ESP32 uses **USB Serial** (one JSON payload per line).
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

## Host Setup (Windows)

1) Ensure `gh` is logged in:
```powershell
gh auth status
```

2) Install the required Python package:
```powershell
pip install pyserial>=3.5
```

3) Start the server (it will list COM ports for selection):
```powershell
python host\server.py
```

The server starts two endpoints:
- **HTTP** `http://<host-ip>:8732/copilot-usage` (for browsers / curl)
- **HTTP** `http://<host-ip>:8732/refresh` (triggers an immediate `gh api` refresh)

Optional debug logging:
```powershell
$env:COPILOT_USAGE_DEBUG="1"
python host\server.py
```

## ESP32 Firmware Setup

1) Open `firmware/CopilotUsageDisplay/CopilotUsageDisplay.ino`.

2) (Optional) Set I2C pins if your board needs them:
```cpp
static const int I2C_SDA = 21;
static const int I2C_SCL = 22;
```

3) Upload to the ESP32.

## Configuration

Two compile-time toggles in `CopilotUsageDisplay.ino`:

```cpp
static const bool SHOW_REMAINING = true;   // true=remaining/total, false=used/total
static const bool FLIP_DISPLAY   = false;  // true=180° rotation
```

- **SHOW_REMAINING**: Set to `true` to display `remaining / total` quota, or `false` to display `used / total`.
- **FLIP_DISPLAY**: Set to `true` to rotate the image 180° (useful when the OLED is mounted upside-down).

Host environment flags:

- `COPILOT_USAGE_SERIAL_PORT` - Serial port name (e.g. `COM6`). If unset, the server prompts.
- `COPILOT_USAGE_SERIAL_BAUD` - Serial baud rate (default `115200`).
- `COPILOT_USAGE_SERIAL_ECHO` - `1` to log each JSON payload sent to serial.
- `COPILOT_USAGE_MONTHLY_QUOTA` - Monthly quota override (default `300`).
- `COPILOT_USAGE_DEBUG` - `1` to enable verbose server logging.

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
- If the USB serial link disconnects, reconnect the cable and restart the host script.
- If you see resets with `Brownout detector was triggered`, use a stronger power supply and a short USB cable.
- Keep secrets out of the repo.

## Files

- `host/server.py` - Python server (HTTP + USB serial) using `gh api`.
- `host/requirements.txt` - Python dependencies.
- `firmware/CopilotUsageDisplay/CopilotUsageDisplay.ino` - ESP32 firmware.
