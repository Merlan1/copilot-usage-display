# Copilot Usage + Claude Max OLED (ESP32 + SSD1306)

## Disclaimer

Slopped together by an AI after a brief discussion about wanting to see Copilot usage on a tiny screen. Almost none of this was written by a human. It mostly works, which is frankly surprising.

## Overview

- Host runs a Python server that calls `gh api` for Copilot usage and the Anthropic OAuth API for Claude Max usage.
- Communication to the ESP32 uses **USB Serial** (one JSON payload per line).
- An HTTP endpoint (`/copilot-usage`) remains available for compatibility.
- ESP32 receives data in real time and shows usage on an SSD1306 OLED.
- A **physical button** (GPIO 0 / BOOT) triggers an immediate refresh.
- Cycles between **Copilot** and **Claude Max** pages every 30 seconds.
- **Per-model breakdown** and **Claude usage bars** with reset countdowns.
- **Automatic serial reconnection** — unplug and replug the ESP32 without restarting the server.

## Requirements

- Windows PC with GitHub CLI (`gh`) installed and authenticated.
- Python 3.8+.
- ESP32 board.
- SSD1306 OLED (128x32 recommended).
- [PlatformIO](https://platformio.org/) (`pip install platformio`) for building the firmware.
- Claude Code CLI (optional — for Claude Max usage tracking).

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

### Autostart as Scheduled Task

```powershell
# Install (run as Administrator):
.\install-service.ps1

# Start immediately without logging out:
Start-ScheduledTask -TaskName 'CopilotUsageDisplayServer'

# Remove (run as Administrator):
.\uninstall-service.ps1
```

The task starts hidden at logon, restarts on failure (up to 3 tries), and logs to `server.log`.

If you need serial output, set the env var before installing:
```powershell
[Environment]::SetEnvironmentVariable('COPILOT_USAGE_SERIAL_PORT', 'COM3', 'User')
```

### Claude Max

If you have [Claude Code](https://docs.anthropic.com/en/docs/claude-code/overview) installed and logged in, the server automatically detects `~/.claude/.credentials.json` and fetches Claude Max usage alongside Copilot. No additional configuration needed.

## ESP32 Firmware Setup

Built with PlatformIO:

```powershell
cd firmware
pio run -t upload
```

For manual serial monitor:
```powershell
pio device monitor -b 115200
```

### PlatformIO project structure

```
firmware/
├── platformio.ini          # board=esp32dev, framework=arduino
├── src/main.cpp            # firmware source (converted from .ino)
├── include/
└── lib/
```

The original `CopilotUsageDisplay.ino` is also kept in `firmware/CopilotUsageDisplay/` for Arduino IDE users.

## Configuration

Two compile-time toggles in `src/main.cpp`:

```cpp
static const bool SHOW_REMAINING = true;   // true=remaining/total, false=used/total
static const bool FLIP_DISPLAY   = false;  // true=180° rotation
```

- **SHOW_REMAINING**: Set to `true` to display `remaining / total` quota, or `false` to display `used / total`.
- **FLIP_DISPLAY**: Set to `true` to rotate the image 180° (useful when the OLED is mounted upside-down).

Host environment flags:

- `COPILOT_USAGE_SERIAL_PORT` — Serial port name (e.g. `COM6`). If unset, the server prompts (or skips serial when running as a service).
- `COPILOT_USAGE_SERIAL_BAUD` — Serial baud rate (default `115200`).
- `COPILOT_USAGE_SERIAL_ECHO` — `1` to log each JSON payload sent to serial.
- `COPILOT_USAGE_MONTHLY_QUOTA` — Monthly quota override (default `300`).
- `COPILOT_USAGE_DEBUG` — `1` to enable verbose server logging.
- `CLAUDE_CREDENTIALS_PATH` — Path to Claude Code credentials (default `~/.claude/.credentials.json`).

## Display Layout

Cycles between two pages every 30 seconds:

### Page 1 — Copilot Usage
```
Used: 50/300
[████████░░░░░░░░░░░░]   16.7%
avg/day 1.7 td 50
```
- **Top row**: title (left) + used/remaining quota (right).
- **Middle row**: progress bar + usage percentage.
- **Bottom row**: cycles through `avg/day` and per‑model percentages every 10 seconds with a smooth vertical slide animation.

### Page 2 — Claude Max
```
Claude Usage
5h [████░░░░░░░░░░░░░░]    9%
7d [██░░░░░░░░░░░░░░░░]    1%
5h reset: 1h30m
```
- **Top row**: page title.
- **Middle rows**: 5-hour and 7-day utilization bars.
- **Bottom row**: countdown until the 5-hour window resets.

## Button

Press the **BOOT button** (GPIO 0) on the ESP32 to send a `refresh` command to the host. The host immediately fetches fresh Copilot usage data from the GitHub API and pushes the update to all connected displays.

## Notes

- The display is configured for 128x32. If you have 128x64, update `SCREEN_HEIGHT`.
- Data is pushed automatically when the server refreshes (every 15 minutes by default). No polling needed.
- **Serial reconnection is automatic** — unplugging and replugging the ESP32 is handled seamlessly.
- If you see resets with `Brownout detector was triggered`, use a stronger power supply and a short USB cable.
- Keep secrets out of the repo.

## Files

- `host/server.py` — Python server (HTTP + USB serial) with Copilot and Claude Max polling.
- `host/requirements.txt` — Python dependencies.
- `firmware/platformio.ini` — PlatformIO project config.
- `firmware/src/main.cpp` — ESP32 firmware source.
- `start-server.ps1` — Launch script for background / service use.
- `install-service.ps1` — Registers a Windows scheduled task (run as Administrator).
- `uninstall-service.ps1` — Removes the scheduled task.
