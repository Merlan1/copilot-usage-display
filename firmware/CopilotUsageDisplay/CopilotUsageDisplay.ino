#include <Arduino.h>
#include <ArduinoJson.h>
#include <math.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// Button (pulls LOW when pressed)
static const uint8_t BTN_PIN = 0;
static const unsigned long DEBOUNCE_MS = 50;
static const unsigned long BUTTON_COOLDOWN_MS = 1000;

// Display mode
static const bool SHOW_REMAINING = false;  // true=remaining/total, false=used/total
static const bool FLIP_DISPLAY = false;    // true=180° rotation

// Timing
static const unsigned long BOOT_DELAY_MS = 1000UL;
static const unsigned long DISPLAY_REFRESH_MS = 30UL;

// Display
static const uint8_t SCREEN_WIDTH = 128;
static const uint8_t SCREEN_HEIGHT = 32;
static const int OLED_RESET = -1;
static const uint8_t SCREEN_ADDRESS = 0x3c;
// Layout offsets — scale with screen height so 32px and 64px both work
static const int8_t TITLE_Y = 0;
static const int8_t BAR_Y = SCREEN_HEIGHT / 4;       // 8 (32px) or 16 (64px)
static const int8_t BAR_H = 6;
static const int8_t INFO_Y = SCREEN_HEIGHT / 2;       // 16 (32px) or 32 (64px)
static const int8_t ERR_Y = SCREEN_HEIGHT / 2;         // 16 (32px) or 32 (64px)
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

// Model breakdown -----------------------------------------------------
static const int MAX_MODELS = 6;
struct ModelEntry {
  String name;
  float percent;
};

static int g_modelCount = 0;
static ModelEntry g_models[MAX_MODELS];

// Model cycling / smooth scroll
static const unsigned long CYCLE_MS = 10000;
static const unsigned long SCROLL_MS = 250;
static int g_page = 0;           // 0=avg/day, 1..N=model
static unsigned long g_lastPageSwitch = 0;
static bool g_scrolling = false;
static unsigned long g_scrollStart = 0;

// ---------------------------------------------------------------------

static bool g_serialConnected = false;
static UsageData g_usage;
static unsigned long g_lastUpdate = 0;
static unsigned long g_lastJitter = 0;
static int g_jitterX = 0;
static int g_jitterY = 0;
static unsigned long g_lastDisplay = 0;

static unsigned long g_lastButtonPress = 0;
static int g_lastButtonState = HIGH;
static String g_serialLine;
static int g_braceDepth = 0;
static bool g_inString = false;
static bool g_escapeNext = false;
static unsigned long g_rxBytes = 0;
static bool g_seenBrace = false;
static uint8_t g_lastByte = 0;
static size_t g_lastBadLen = 0;
static bool g_lastBadFraming = false;
static String g_lastBadPrefix;

static void updateJitter() {
  unsigned long now = millis();
  if (g_lastJitter == 0 || now - g_lastJitter >= 5000) {
    g_lastJitter = now;
    g_jitterX = (g_jitterX == 0) ? 1 : 0;
    g_jitterY = (g_jitterY == 0) ? 1 : 0;
  }
}

static void parseModels(const JsonDocument &doc) {
  g_modelCount = 0;
  JsonArrayConst arr = doc["models"];
  for (JsonVariantConst m : arr) {
    if (g_modelCount >= MAX_MODELS) break;
    const char *name = m["model"].as<const char *>();
    g_models[g_modelCount].name = String(name ? name : "?");
    g_models[g_modelCount].percent = m["percent"] | 0.0f;
    g_modelCount++;
  }
  if (g_modelCount > 0) {
    g_page = g_page % (g_modelCount + 1);
  } else {
    g_page = 0;
  }
}

static bool parseJsonPayload(const String &payload, UsageData &out) {
  g_lastBadLen = 0;
  g_lastBadFraming = false;
  g_lastBadPrefix = "";
  String trimmed = payload;
  trimmed.trim();
  if (!trimmed.startsWith("{") || !trimmed.endsWith("}")) {
    int first = trimmed.indexOf('{');
    int last = trimmed.lastIndexOf('}');
    if (first >= 0 && last > first) {
      trimmed = trimmed.substring(first, last + 1);
    }
  }
  if (!trimmed.startsWith("{") || !trimmed.endsWith("}")) {
    out.error = "json";
    out.hasData = false;
    g_lastBadLen = payload.length();
    g_lastBadFraming = true;
    g_lastBadPrefix = trimmed.substring(0, 12);
    return false;
  }

  size_t capacity = payload.length() * 2 + 1024;
  if (capacity < 4096) capacity = 4096;
  if (capacity > 16384) capacity = 16384;
  DynamicJsonDocument doc(capacity);
  DeserializationError err = deserializeJson(doc, trimmed);
  if (err) {
    out.error = "parse";
    out.hasData = false;
    g_lastBadLen = trimmed.length();
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

  parseModels(doc);
  return true;
}

static void pollSerial() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    g_rxBytes++;
    g_lastByte = (uint8_t)c;
    if (c == '{') {
      g_seenBrace = true;
    }
    if (g_braceDepth == 0) {
      if (c == '{') {
        g_serialLine = "{";
        g_braceDepth = 1;
        g_inString = false;
        g_escapeNext = false;
      }
      continue;
    }

    if (g_serialLine.length() < 16384) {
      g_serialLine += c;
    }

    if (g_escapeNext) {
      g_escapeNext = false;
      continue;
    }
    if (g_inString && c == '\\') {
      g_escapeNext = true;
      continue;
    }
    if (c == '"') {
      g_inString = !g_inString;
      continue;
    }
    if (g_inString) {
      continue;
    }

    if (c == '{') {
      g_braceDepth++;
    } else if (c == '}') {
      g_braceDepth--;
      if (g_braceDepth == 0) {
        g_lastUpdate = millis();
        g_serialConnected = true;
        parseJsonPayload(g_serialLine, g_usage);
        g_serialLine = "";
      }
    }
  }
}

static void drawProgressBar(int x, int y, int w, int h, float percent) {
  display.drawRect(x, y, w, h, SSD1306_WHITE);
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  int fill = (int)((percent / 100.0f) * (w - 2));
  display.fillRect(x + 1, y + 1, fill, h - 2, SSD1306_WHITE);
}

static void drawInfoLine(int page, int yOff) {
  int y = INFO_Y + g_jitterY + yOff;
  display.setCursor(0 + g_jitterX, y);
  if (page == 0) {
    display.print("avg/day ");
    if (!isnan(g_usage.avgDaily)) display.print(g_usage.avgDaily, 1);
    else display.print("-");
    display.print(" td ");
    if (!isnan(g_usage.todayUsed)) display.print(g_usage.todayUsed, 0);
    else display.print("-");
  } else {
    int idx = page - 1;
    if (idx < g_modelCount) {
      display.print("M:");
      display.print(g_models[idx].name);
      display.print(" ");
      display.print(g_models[idx].percent, 1);
      display.print("%");
    }
  }
}

static void renderDisplay() {
  updateJitter();
  unsigned long now = millis();
  display.clearDisplay();

  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  if (!g_usage.hasData) {
    const char *status = "-";
    String error = g_usage.error;
    error.toLowerCase();
    if (error.indexOf("http") >= 0) {
      status = "http";
    } else if (error.indexOf("offline") >= 0) {
      status = "serial";
    } else if (error.indexOf("parse") >= 0) {
      status = "json";
    } else if (error.indexOf("auth") >= 0 || error.indexOf("401") >= 0) {
      status = "auth";
    }

    display.setTextSize(1);
    display.setCursor(0 + g_jitterX, 0 + g_jitterY);
    display.print("NO DATA");
    display.print(" (");
    display.print(status);
    display.print(")");
    display.setCursor(0 + g_jitterX, 8 + g_jitterY);
    if (g_lastUpdate == 0) {
      if (g_rxBytes > 0) {
        display.print("rx");
        display.print((int)g_rxBytes);
        display.print(" b");
        if (g_lastByte < 16) display.print("0");
        display.print(g_lastByte, HEX);
        display.print(g_seenBrace ? " o1" : " o0");
      } else {
        display.print("waiting");
      }
    } else if (g_lastBadFraming) {
      display.print("rx ");
      display.print((int)g_lastBadLen);
    } else {
      display.print("bad json ");
      display.print((int)g_lastBadLen);
    }
    display.display();
    return;
  }

  display.setCursor(0 + g_jitterX, TITLE_Y + g_jitterY);
  display.print("Usage:");

  // Used/remaining at top right
  display.setCursor(75 + g_jitterX, TITLE_Y + g_jitterY);
  if (SHOW_REMAINING) {
    display.print((int)g_usage.remaining);
  } else {
    display.print((int)(g_usage.limit - g_usage.remaining));
  }
  display.print("/");
  display.print((int)g_usage.limit);

  // Bottom cycling info line
  if (g_modelCount > 0) {
    int totalPages = g_modelCount + 1;

    if (!g_scrolling && g_lastPageSwitch > 0 && now - g_lastPageSwitch >= CYCLE_MS) {
      g_scrolling = true;
      g_scrollStart = now;
    }

    if (g_scrolling) {
      unsigned long elapsed = now - g_scrollStart;
      if (elapsed >= SCROLL_MS) {
        g_scrolling = false;
        g_page = (g_page + 1) % totalPages;
        g_lastPageSwitch = now;
        drawInfoLine(g_page, 0);
      } else {
        float p = (float)elapsed / (float)SCROLL_MS;
        int off = (int)(p * 4);
        drawInfoLine(g_page, -off);
        drawInfoLine((g_page + 1) % totalPages, 4 - off);
      }
    } else {
      if (g_lastPageSwitch == 0) g_lastPageSwitch = now;
      drawInfoLine(g_page, 0);
    }
  } else {
    drawInfoLine(0, 0);
  }

  // Progress bar + percent — erase bar area first to hide scrolled text
  display.fillRect(0 + g_jitterX, BAR_Y + g_jitterY, 128, BAR_H, 0);
  drawProgressBar(0 + g_jitterX, BAR_Y + g_jitterY, 80, BAR_H, g_usage.percent);
  display.setTextSize(1);
  display.setCursor(86 + g_jitterX, BAR_Y + g_jitterY);
  if (!isnan(g_usage.percent)) {
    display.print(g_usage.percent, 1);
    display.print("%");
  } else {
    display.print("--");
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

  if (FLIP_DISPLAY) {
    display.setRotation(2);
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

  pinMode(BTN_PIN, INPUT_PULLUP);
  g_lastButtonState = digitalRead(BTN_PIN);

  Serial.setRxBufferSize(8192);
  Serial.begin(115200);
  Serial.println("ready");
  delay(200);

  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("Copilot Usage");
  display.setCursor(0, 16);
  display.print("Serial OK");
  display.display();

  g_usage.hasData = false;
  g_lastUpdate = 0;
  g_serialConnected = false;
  g_modelCount = 0;
  g_page = 0;
  g_lastPageSwitch = 0;
  g_scrolling = false;
}

void loop() {
  pollSerial();

  int buttonState = digitalRead(BTN_PIN);
  if (buttonState == LOW && g_lastButtonState == HIGH && millis() - g_lastButtonPress >= BUTTON_COOLDOWN_MS) {
    g_lastButtonPress = millis();
    Serial.println("refresh");
  }
  g_lastButtonState = buttonState;

  if (g_lastUpdate == 0 && (millis() % 2000UL) < 30) {
    Serial.println("ready");
  }

  unsigned long now = millis();
  if (g_lastDisplay == 0 || now - g_lastDisplay >= DISPLAY_REFRESH_MS) {
    g_lastDisplay = now;
    renderDisplay();
  }
}
