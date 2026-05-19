#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "secrets.h"

// Wi-Fi
static const char *WIFI_SSID = SECRET_SSID;
static const char *WIFI_PASS = SECRET_PASSWORD;

// HTTP endpoint (host running server.py)
static const char *USAGE_URL = "http://192.168.1.171:8732/copilot-usage";

// Polling
static const unsigned long POLL_INTERVAL_MS = 5UL * 60UL * 1000UL;
static const unsigned long BOOT_DELAY_MS = 3000UL;
static const unsigned long DISPLAY_REFRESH_MS = 500UL;

// Display
static const uint8_t SCREEN_WIDTH = 128;
static const uint8_t SCREEN_HEIGHT = 32;
static const int OLED_RESET = -1;
static const uint8_t SCREEN_ADDRESS = 0x3c;
// I2C pins (set to -1 to use board defaults)
static const int I2C_SDA = -1;
static const int I2C_SCL = -1;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

struct UsageData {
  float remaining;
  float limit;
  float percent;
  float avgDaily;
  float todayUsed;
  long pollIntervalSec;
  long pollInSec;
  String resetAt;
  String updatedAt;
  String nextPollAt;
  String error;
  bool hasData;
};

static UsageData g_usage;
static unsigned long g_lastPoll = 0;
static unsigned long g_lastJitter = 0;
static int g_jitterX = 0;
static int g_jitterY = 0;
static unsigned long g_lastDisplay = 0;
static unsigned long g_lastCountdownToggle = 0;
static bool g_showServerCountdown = false;

static void updateJitter() {
  unsigned long now = millis();
  if (g_lastJitter == 0 || now - g_lastJitter >= 5000) {
    g_lastJitter = now;
    g_jitterX = (g_jitterX == 0) ? 1 : 0;
    g_jitterY = (g_jitterY == 0) ? 1 : 0;
  }
}

static void updateCountdownToggle() {
  unsigned long now = millis();
  if (g_lastCountdownToggle == 0 || now - g_lastCountdownToggle >= 3000) {
    g_lastCountdownToggle = now;
    g_showServerCountdown = !g_showServerCountdown;
  }
}

static long deviceCountdownSeconds() {
  if (g_lastPoll == 0) {
    return -1;
  }
  long interval_sec = (long)(POLL_INTERVAL_MS / 1000UL);
  long elapsed = (long)((millis() - g_lastPoll) / 1000UL);
  long remaining = interval_sec - elapsed;
  if (remaining < 0) remaining = 0;
  return remaining;
}

static long serverCountdownSeconds() {
  if (g_usage.pollInSec < 0 || g_lastPoll == 0) {
    return -1;
  }
  long elapsed = (long)((millis() - g_lastPoll) / 1000UL);
  long remaining = g_usage.pollInSec - elapsed;
  if (remaining < 0) remaining = 0;
  return remaining;
}

static void drawCircleFillMeter(int cx, int cy, int r, float ratio) {
  if (ratio < 0.0f) ratio = 0.0f;
  if (ratio > 1.0f) ratio = 1.0f;

  display.drawCircle(cx, cy, r, SSD1306_WHITE);

  int fill_height = (int)roundf((float)(2 * r) * ratio);
  if (fill_height <= 0) {
    return;
  }

  int y_bottom = cy + r;
  int y_top = y_bottom - fill_height + 1;
  if (y_top < cy - r) {
    y_top = cy - r;
  }

  for (int y = y_bottom; y >= y_top; --y) {
    int dy = y - cy;
    int dx = (int)floorf(sqrtf((float)(r * r - dy * dy)));
    display.drawFastHLine(cx - dx, y, 2 * dx + 1, SSD1306_WHITE);
  }
}

static void drawTopRightIndicators() {
  long device_remaining = deviceCountdownSeconds();
  long server_remaining = serverCountdownSeconds();
  float device_ratio = 0.0f;
  float server_ratio = 0.0f;

  if (device_remaining >= 0) {
    float interval = (float)(POLL_INTERVAL_MS / 1000UL);
    device_ratio = (interval - (float)device_remaining) / interval;
  }
  if (server_remaining >= 0 && g_usage.pollIntervalSec > 0) {
    float interval = (float)g_usage.pollIntervalSec;
    server_ratio = (interval - (float)server_remaining) / interval;
  }

  int r = 5;
  int y = 5 + g_jitterY;
  int x1 = 108 + g_jitterX;
  int x2 = 120 + g_jitterX;
  drawCircleFillMeter(x1, y, r, device_ratio);
  drawCircleFillMeter(x2, y, r, server_ratio);
}

static void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
  }
}

static bool parseJsonPayload(const String &payload, UsageData &out) {
  StaticJsonDocument<768> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    out.error = "parse";
    out.hasData = false;
    return false;
  }

  out.remaining = doc["remaining"] | NAN;
  out.limit = doc["limit"] | NAN;
  out.percent = doc["percent"] | NAN;
  out.avgDaily = doc["avgDaily"] | NAN;
  out.todayUsed = doc["todayUsed"] | NAN;
  out.pollIntervalSec = doc["pollIntervalSec"] | 0;
  out.pollInSec = doc["pollInSec"] | -1;
  const char *reset_at = doc["resetAt"].as<const char *>();
  const char *updated_at = doc["updatedAt"].as<const char *>();
  const char *next_poll_at = doc["nextPollAt"].as<const char *>();
  const char *error_text = doc["error"].as<const char *>();
  out.resetAt = String(reset_at ? reset_at : "");
  out.updatedAt = String(updated_at ? updated_at : "");
  out.nextPollAt = String(next_poll_at ? next_poll_at : "");
  out.error = String(error_text ? error_text : "");
  out.hasData = isnan(out.remaining) == false && isnan(out.limit) == false;

  return true;
}

static void fetchUsage() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  if (WiFi.status() != WL_CONNECTED) {
    g_usage.error = "offline";
    g_usage.hasData = false;
    return;
  }

  HTTPClient http;
  http.setTimeout(7000);
  http.begin(USAGE_URL);

  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    parseJsonPayload(body, g_usage);
  } else {
    g_usage.error = "http";
    g_usage.hasData = false;
  }
  http.end();
}

static void drawProgressBar(int x, int y, int w, int h, float percent) {
  display.drawRect(x, y, w, h, SSD1306_WHITE);
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  int fill = (int)((percent / 100.0f) * (w - 2));
  display.fillRect(x + 1, y + 1, fill, h - 2, SSD1306_WHITE);
}

static void renderDisplay() {
  updateJitter();
  display.clearDisplay();

  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0 + g_jitterX, 0 + g_jitterY);
  display.print("Copilot Usage");

  drawTopRightIndicators();

  if (!g_usage.hasData) {
    const char *status = "-";
    String error = g_usage.error;
    error.toLowerCase();
    if (error.indexOf("http") >= 0) {
      status = "http";
    } else if (error.indexOf("offline") >= 0) {
      status = "wifi";
    } else if (error.indexOf("parse") >= 0) {
      status = "json";
    } else if (error.indexOf("auth") >= 0 || error.indexOf("401") >= 0) {
      status = "auth";
    }

    display.setTextSize(1);
    display.setCursor(0 + g_jitterX, 16 + g_jitterY);
    display.print("NO DATA");
    display.print(" (");
    display.print(status);
    display.print(")");
    display.display();
    return;
  }

  display.setTextSize(1);
  display.setCursor(0 + g_jitterX, 8 + g_jitterY);
  display.print((int)g_usage.remaining);
  display.print("/");
  display.print((int)g_usage.limit);

  drawProgressBar(0 + g_jitterX, 18 + g_jitterY, 80, 6, g_usage.percent);
  display.setTextSize(1);
  display.setCursor(86 + g_jitterX, 18 + g_jitterY);
  if (!isnan(g_usage.percent)) {
    display.print(g_usage.percent, 1);
    display.print("%");
  } else {
    display.print("--");
  }

  display.setCursor(0 + g_jitterX, 24 + g_jitterY);
  display.print("avg/day ");
  if (!isnan(g_usage.avgDaily)) {
    display.print(g_usage.avgDaily, 1);
  } else {
    display.print("-");
  }

  display.print(" td ");
  if (!isnan(g_usage.todayUsed)) {
    display.print(g_usage.todayUsed, 0);
  } else {
    display.print("-");
  }

  display.display();
}

void setup() {
  if (I2C_SDA >= 0 && I2C_SCL >= 0) {
    Wire.begin(I2C_SDA, I2C_SCL);
  } else {
    Wire.begin();
  }

  delay(500);

  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    for (;;) {
      delay(1000);
    }
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("Copilot Usage");
  display.setCursor(0, 16);
  display.print("Display OK");
  display.display();

  delay(BOOT_DELAY_MS);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  connectWiFi();

  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("Copilot Usage");
  display.setCursor(0, 16);
  if (WiFi.status() == WL_CONNECTED) {
    display.print("WiFi OK");
  } else {
    display.print("WiFi FAIL");
  }
  display.display();

  g_usage.hasData = false;
  g_lastPoll = 0;
  g_lastCountdownToggle = 0;
}

void loop() {
  unsigned long now = millis();
  if (g_lastPoll == 0 || now - g_lastPoll >= POLL_INTERVAL_MS) {
    g_lastPoll = now;
    fetchUsage();
  }
  if (g_lastDisplay == 0 || now - g_lastDisplay >= DISPLAY_REFRESH_MS) {
    g_lastDisplay = now;
    renderDisplay();
  }
}
