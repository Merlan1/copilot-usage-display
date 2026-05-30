#include <Arduino.h>
#include <ArduinoJson.h>
#include <math.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// Button (pulls LOW when pressed)
static const uint8_t BTN_PIN = 0;
static const unsigned long BUTTON_COOLDOWN_MS = 1000;

// Display mode
static const bool SHOW_REMAINING = false;       // true=remaining/total, false=used/total
static const bool FLIP_DISPLAY = false;         // true=180° rotation
static const bool SHOW_COPILOT_SCREEN = false;  // show Copilot usage screen
static const bool SHOW_CLAUDE_SCREEN = true;    // show Claude Max usage screen

// Timing
static const unsigned long BOOT_DELAY_MS = 1000UL;
static const unsigned long DISPLAY_REFRESH_MS = 30UL;
static const unsigned long PAGE_SWITCH_MS = 30000UL;
static const unsigned long SERIAL_TIMEOUT_MS = 120000UL; // 2 min without data → display sleep
static const unsigned long CYCLE_MS = 10000;
static const unsigned long SCROLL_MS = 250;

// Display
static const uint8_t SCREEN_WIDTH = 128;
static const uint8_t SCREEN_HEIGHT = 32;
static const int OLED_RESET = -1;
static const uint8_t SCREEN_ADDRESS = 0x3c;
// Layout offsets — scale with screen height so 32px and 64px both work
static const int8_t TITLE_Y = 0;
static const int8_t BAR_Y = SCREEN_HEIGHT / 4; // 8 (32px) or 16 (64px)
static const int8_t BAR_H = 6;
static const int8_t INFO_Y = SCREEN_HEIGHT / 2; // 16 (32px) or 32 (64px)
// I2C pins (set to -1 to use board defaults)
static const int I2C_SDA = -1;
static const int I2C_SCL = -1;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

struct ClaudeData
{
  float fiveHour;
  float sevenDay;
  float sevenDaySonnet;
  float sevenDayOpus;
  long fiveHourResetsInSec;
  long sevenDayResetsInSec;
  bool hasData;
};

struct UsageData
{
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
  ClaudeData claude;
};

static const int MAX_MODELS = 6;
struct ModelEntry
{
  String name;
  float percent;
};

// Serial JSON frame parser
struct SerialParser
{
  String line;
  int braceDepth = 0;
  bool inString = false;
  bool escapeNext = false;
  unsigned long rxBytes = 0;
  bool seenBrace = false;
  uint8_t lastByte = 0;
  size_t lastBadLen = 0;
  bool lastBadFraming = false;
  String lastBadPrefix;
  bool connected = false;
};

// Bottom info-line cycling (avg/day → model 1 → model N → …)
struct ScrollState
{
  int page = 0; // 0=avg/day, 1..N=model
  unsigned long lastSwitch = 0;
  bool active = false;
  unsigned long startTime = 0;
};

// Top-level display page (Copilot vs Claude) and sleep tracking
struct ScreenState
{
  int page = 0;
  unsigned long lastSwitch = 0;
  bool asleep = false;
  unsigned long lastDataTime = 0; // millis() of last valid packet; 0 until first packet
  unsigned long lastUpdate = 0;
  unsigned long lastRender = 0;
};

// Pixel jitter to reduce OLED burn-in
struct Jitter
{
  int x = 0;
  int y = 0;
  unsigned long lastTime = 0;
};

struct ButtonState
{
  unsigned long lastPress = 0;
  int lastState = HIGH;
};

static int g_modelCount = 0;
static ModelEntry g_models[MAX_MODELS];

static SerialParser g_serial;
static ScrollState g_scroll;
static ScreenState g_screen;
static Jitter g_jitter;
static ButtonState g_button;
static UsageData g_usage;

static void updateJitter()
{
  unsigned long now = millis();
  if (g_jitter.lastTime == 0 || now - g_jitter.lastTime >= 5000)
  {
    g_jitter.lastTime = now;
    g_jitter.x = (g_jitter.x == 0) ? 1 : 0;
    g_jitter.y = (g_jitter.y == 0) ? 1 : 0;
  }
}

static void parseModels(const JsonDocument &doc)
{
  g_modelCount = 0;
  JsonArrayConst arr = doc["models"];
  for (JsonVariantConst m : arr)
  {
    if (g_modelCount >= MAX_MODELS)
      break;
    const char *name = m["model"].as<const char *>();
    g_models[g_modelCount].name = String(name ? name : "?");
    g_models[g_modelCount].percent = m["percent"] | 0.0f;
    g_modelCount++;
  }
  g_scroll.page = (g_modelCount > 0) ? g_scroll.page % (g_modelCount + 1) : 0;
}

static bool parseJsonPayload(const String &payload, UsageData &out)
{
  g_serial.lastBadLen = 0;
  g_serial.lastBadFraming = false;
  g_serial.lastBadPrefix = "";
  String trimmed = payload;
  trimmed.trim();
  if (!trimmed.startsWith("{") || !trimmed.endsWith("}"))
  {
    int first = trimmed.indexOf('{');
    int last = trimmed.lastIndexOf('}');
    if (first >= 0 && last > first)
      trimmed = trimmed.substring(first, last + 1);
  }
  if (!trimmed.startsWith("{") || !trimmed.endsWith("}"))
  {
    out.error = "json";
    out.hasData = false;
    g_serial.lastBadLen = payload.length();
    g_serial.lastBadFraming = true;
    g_serial.lastBadPrefix = trimmed.substring(0, 12);
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, trimmed);
  if (err)
  {
    out.error = "parse";
    out.hasData = false;
    g_serial.lastBadLen = trimmed.length();
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
  out.hasData = !isnan(out.remaining) && !isnan(out.limit);

  JsonVariantConst claude = doc["claude"];
  if (!claude.isNull())
  {
    JsonVariantConst fh = claude["five_hour"];
    if (!fh.isNull())
    {
      out.claude.fiveHour = fh["utilization"] | NAN;
      out.claude.fiveHourResetsInSec = fh["resetsInSec"] | -1;
      out.claude.sevenDay = claude["seven_day"]["utilization"] | NAN;
      out.claude.sevenDayResetsInSec = claude["seven_day"]["resetsInSec"] | -1;
      out.claude.sevenDaySonnet = claude["seven_day_sonnet"]["utilization"] | NAN;
      out.claude.sevenDayOpus = claude["seven_day_opus"]["utilization"] | NAN;
      out.claude.hasData = !isnan(out.claude.fiveHour);
    }
  }
  else
  {
    out.claude.hasData = false;
  }

  parseModels(doc);
  return true;
}

static void pollSerial()
{
  while (Serial.available() > 0)
  {
    char c = (char)Serial.read();
    g_serial.rxBytes++;
    g_serial.lastByte = (uint8_t)c;
    if (c == '{')
      g_serial.seenBrace = true;
    if (g_serial.braceDepth == 0)
    {
      if (c == '{')
      {
        g_serial.line = "{";
        g_serial.braceDepth = 1;
        g_serial.inString = false;
        g_serial.escapeNext = false;
      }
      continue;
    }

    if (g_serial.line.length() < 16384)
      g_serial.line += c;

    if (g_serial.escapeNext)
    {
      g_serial.escapeNext = false;
      continue;
    }
    if (g_serial.inString && c == '\\')
    {
      g_serial.escapeNext = true;
      continue;
    }
    if (c == '"')
    {
      g_serial.inString = !g_serial.inString;
      continue;
    }
    if (g_serial.inString)
      continue;

    if (c == '{')
    {
      g_serial.braceDepth++;
    }
    else if (c == '}')
    {
      g_serial.braceDepth--;
      if (g_serial.braceDepth == 0)
      {
        g_screen.lastUpdate = millis();
        g_screen.lastDataTime = g_screen.lastUpdate;
        g_serial.connected = true;
        parseJsonPayload(g_serial.line, g_usage);
        g_serial.line = "";
      }
    }
  }
}

static void drawProgressBar(int x, int y, int w, int h, float percent)
{
  display.drawRect(x, y, w, h, SSD1306_WHITE);
  if (percent < 0)
    percent = 0;
  if (percent > 100)
    percent = 100;
  int fill = (int)((percent / 100.0f) * (w - 2));
  display.fillRect(x + 1, y + 1, fill, h - 2, SSD1306_WHITE);
}

static void drawInfoLine(int page, int yOff, int claudePages)
{
  int y = INFO_Y + g_jitter.y + yOff;
  display.setCursor(0 + g_jitter.x, y);
  if (page == 0)
  {
    display.print("avg/day ");
    if (!isnan(g_usage.avgDaily))
      display.print(g_usage.avgDaily, 1);
    else
      display.print("-");
    display.print(" td ");
    if (!isnan(g_usage.todayUsed))
      display.print(g_usage.todayUsed, 0);
    else
      display.print("-");
  }
  else if (page <= g_modelCount)
  {
    int idx = page - 1;
    if (idx < g_modelCount)
    {
      display.print("M:");
      display.print(g_models[idx].name);
      display.print(" ");
      display.print(g_models[idx].percent, 1);
      display.print("%");
    }
  }
  else if (claudePages > 0)
  {
    display.print("Cl:");
    if (!isnan(g_usage.claude.fiveHour))
    {
      display.print("5h ");
      display.print((int)g_usage.claude.fiveHour);
      display.print("%");
    }
    if (!isnan(g_usage.claude.sevenDay))
    {
      display.print(" 7d ");
      display.print((int)g_usage.claude.sevenDay);
      display.print("%");
    }
  }
}

static void renderClaudeDisplay()
{
  updateJitter();
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(0 + g_jitter.x, 0 + g_jitter.y);
  display.print("Claude Usage");

  int barX = 24;
  int barW = 75;
  int barH = 5;
  int labelX = 0 + g_jitter.x;
  int pctX = 103 + g_jitter.x;
  int positions[] = {9, 18};

  struct
  {
    const char *label;
    float value;
  } bars[2];
  bars[0] = {"5h", g_usage.claude.fiveHour};
  bars[1] = {"7d", g_usage.claude.sevenDay};

  for (int i = 0; i < 2; i++)
  {
    int y = positions[i] + g_jitter.y;
    display.setCursor(labelX, y);
    display.print(bars[i].label);
    drawProgressBar(barX + g_jitter.x, y, barW, barH, bars[i].value);
    display.setCursor(pctX, y);
    if (!isnan(bars[i].value))
    {
      display.print((int)bars[i].value);
      display.print("%");
    }
    else
    {
      display.print("--");
    }
  }

  display.setCursor(0 + g_jitter.x, 25 + g_jitter.y);
  long secs = g_usage.claude.fiveHourResetsInSec;
  if (secs > 0)
  {
    int h = secs / 3600;
    int m = (secs % 3600) / 60;
    display.print("5h reset: ");
    display.print(h);
    display.print("h");
    if (m < 10)
      display.print("0");
    display.print(m);
    display.print("m");
  }

  display.display();
}

static void renderNoData()
{
  const char *status = "-";
  String error = g_usage.error;
  error.toLowerCase();
  if (error.indexOf("http") >= 0)
    status = "http";
  else if (error.indexOf("offline") >= 0)
    status = "serial";
  else if (error.indexOf("parse") >= 0)
    status = "json";
  else if (error.indexOf("auth") >= 0 || error.indexOf("401") >= 0)
    status = "auth";

  display.setCursor(0 + g_jitter.x, 0 + g_jitter.y);
  display.print("NO DATA (");
  display.print(status);
  display.print(")");

  display.setCursor(0 + g_jitter.x, 8 + g_jitter.y);
  if (g_screen.lastUpdate == 0)
  {
    if (g_serial.rxBytes > 0)
    {
      display.print("rx");
      display.print((int)g_serial.rxBytes);
      display.print(" b");
      if (g_serial.lastByte < 16)
        display.print("0");
      display.print(g_serial.lastByte, HEX);
      display.print(g_serial.seenBrace ? " o1" : " o0");
    }
    else
    {
      display.print("waiting");
    }
  }
  else if (g_serial.lastBadFraming)
  {
    display.print("rx ");
    display.print((int)g_serial.lastBadLen);
  }
  else
  {
    display.print("bad json ");
    display.print((int)g_serial.lastBadLen);
  }
}

static void renderCopilotDisplay()
{
  updateJitter();
  unsigned long now = millis();
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  if (!g_usage.hasData)
  {
    renderNoData();
    display.display();
    return;
  }

  display.setCursor(0 + g_jitter.x, TITLE_Y + g_jitter.y);
  display.print(SHOW_REMAINING ? "Remaining:" : "Used:");

  display.setCursor(75 + g_jitter.x, TITLE_Y + g_jitter.y);
  display.print((int)(SHOW_REMAINING ? g_usage.remaining : g_usage.limit - g_usage.remaining));
  display.print("/");
  display.print((int)g_usage.limit);

  int modelPages = g_modelCount > 0 ? g_modelCount : 0;
  int claudePages = g_usage.claude.hasData ? 1 : 0;
  int totalPages = 1 + modelPages + claudePages;

  if (totalPages > 1)
  {
    if (!g_scroll.active && g_scroll.lastSwitch > 0 && now - g_scroll.lastSwitch >= CYCLE_MS)
    {
      g_scroll.active = true;
      g_scroll.startTime = now;
    }

    if (g_scroll.active)
    {
      unsigned long elapsed = now - g_scroll.startTime;
      if (elapsed >= SCROLL_MS)
      {
        g_scroll.active = false;
        g_scroll.page = (g_scroll.page + 1) % totalPages;
        g_scroll.lastSwitch = now;
        drawInfoLine(g_scroll.page, 0, claudePages);
      }
      else
      {
        float p = (float)elapsed / (float)SCROLL_MS;
        int off = (int)(p * 4);
        drawInfoLine(g_scroll.page, -off, claudePages);
        drawInfoLine((g_scroll.page + 1) % totalPages, 4 - off, claudePages);
      }
    }
    else
    {
      if (g_scroll.lastSwitch == 0)
        g_scroll.lastSwitch = now;
      drawInfoLine(g_scroll.page, 0, claudePages);
    }
  }
  else
  {
    drawInfoLine(0, 0, claudePages);
  }

  // Erase bar area to hide scrolled text bleed-through, then draw bar
  display.fillRect(0 + g_jitter.x, BAR_Y + g_jitter.y, 128, BAR_H, 0);
  drawProgressBar(0 + g_jitter.x, BAR_Y + g_jitter.y, 80, BAR_H, g_usage.percent);
  display.setTextSize(1);
  display.setCursor(86 + g_jitter.x, BAR_Y + g_jitter.y);
  if (!isnan(g_usage.percent))
  {
    display.print(g_usage.percent, 1);
    display.print("%");
  }
  else
  {
    display.print("--");
  }

  display.display();
}

static void renderDisplay()
{
  unsigned long now = millis();

  unsigned long sinceData = (g_screen.lastDataTime > 0) ? (now - g_screen.lastDataTime) : now;
  bool shouldSleep = sinceData > SERIAL_TIMEOUT_MS;

  if (shouldSleep && !g_screen.asleep)
  {
    display.clearDisplay();
    display.display();
    display.ssd1306_command(SSD1306_DISPLAYOFF);
    g_screen.asleep = true;
    return;
  }
  if (!shouldSleep && g_screen.asleep)
  {
    display.ssd1306_command(SSD1306_DISPLAYON);
    g_screen.asleep = false;
  }
  if (g_screen.asleep)
    return;

  bool showCopilot = SHOW_COPILOT_SCREEN;
  bool showClaude = SHOW_CLAUDE_SCREEN && g_usage.claude.hasData;

  if (showCopilot && showClaude)
  {
    if (g_screen.lastSwitch == 0)
      g_screen.lastSwitch = now;
    if (now - g_screen.lastSwitch >= PAGE_SWITCH_MS)
    {
      g_screen.lastSwitch = now;
      g_screen.page = (g_screen.page + 1) % 2;
    }
  }
  else
  {
    g_screen.page = 0;
    g_screen.lastSwitch = 0;
  }

  if (showClaude && (!showCopilot || g_screen.page == 1))
    renderClaudeDisplay();
  else
    renderCopilotDisplay();
}

void setup()
{
  if (I2C_SDA >= 0 && I2C_SCL >= 0)
    Wire.begin(I2C_SDA, I2C_SCL);
  else
    Wire.begin();

  delay(500);

  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS))
  {
    for (;;)
      delay(1000);
  }

  if (FLIP_DISPLAY)
    display.setRotation(2);

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
  g_button.lastState = digitalRead(BTN_PIN);

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
  g_usage.claude.hasData = false;
  g_usage.claude.fiveHour = NAN;
  g_usage.claude.sevenDay = NAN;
  g_usage.claude.sevenDaySonnet = NAN;
  g_usage.claude.sevenDayOpus = NAN;
  g_usage.claude.fiveHourResetsInSec = -1;
  g_usage.claude.sevenDayResetsInSec = -1;
}

void loop()
{
  pollSerial();

  int buttonState = digitalRead(BTN_PIN);
  if (buttonState == LOW && g_button.lastState == HIGH && millis() - g_button.lastPress >= BUTTON_COOLDOWN_MS)
  {
    g_button.lastPress = millis();
    Serial.println("refresh");
  }
  g_button.lastState = buttonState;

  if (g_screen.lastUpdate == 0 && (millis() % 2000UL) < 30)
    Serial.println("ready");

  unsigned long now = millis();
  if (g_screen.lastRender == 0 || now - g_screen.lastRender >= DISPLAY_REFRESH_MS)
  {
    g_screen.lastRender = now;
    renderDisplay();
  }
}
