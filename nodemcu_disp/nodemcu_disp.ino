#include <ESP8266WiFi.h>
#include <HubMqtt.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "secrets.h"  // WiFi and MQTT logins: copy secrets.example.h

const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// MQTT broker on the webserver PC (webserver/README.md "MQTT").
const char* mqttHost = MQTT_HOST;
const uint16_t mqttPort = 1883;
const char* mqttName = "nodemcu-disp";
const char* mqttPassword = MQTT_PASSWORD;
const bool showAlerts = false;  // subscribe to alerts/# and show them on the OLED

Adafruit_SSD1306 display(128, 64, &Wire, -1);
bool oledReady = false;
HubMqtt hub(mqttHost, mqttPort, mqttName, mqttPassword);

const char* LEVEL_NAMES[] = {"INFO", "WARNING", "CRITICAL"};

// Draw a 10-segment horizontal bargraph along the bottom of the display.
void drawSignalBar(int percent) {
  const int segments = 10;
  const int segWidth = 10;
  const int segPitch = 12;
  const int x0 = (128 - (segments - 1) * segPitch - segWidth) / 2;
  const int y = 46;
  const int h = 16;
  int lit = (percent + 5) / 10;

  for (int i = 0; i < segments; i++) {
    int x = x0 + i * segPitch;
    if (i < lit) {
      display.fillRect(x, y, segWidth, h, SSD1306_WHITE);
    } else {
      display.drawRect(x, y, segWidth, h, SSD1306_WHITE);
    }
  }
}

// Copy UTF-8 text for the display's built-in font (code page 437): ° becomes its degree sign,
// any other non-ASCII character a '?'.
void toDisplayText(char* dst, size_t size, const char* src) {
  size_t n = 0;
  while (*src && n + 1 < size) {
    uint8_t c = *src;
    if (c < 0x80) {
      dst[n++] = c;
      src++;
      continue;
    }
    dst[n++] = (c == 0xC2 && (uint8_t)src[1] == 0xB0) ? (char)248 : '?';
    src++;
    while ((*src & 0xC0) == 0x80) src++;  // the rest of the character
  }
  dst[n] = 0;
}

// Print text word-wrapped to the display's 21 columns, on at most maxLines lines.
void printWrapped(const char* text, int maxLines) {
  const int cols = 21;
  while (*text && maxLines-- > 0) {
    while (*text == ' ') text++;
    int len = strlen(text);
    int n = len;
    if (len > cols) {
      n = cols;
      while (n > 0 && text[n] != ' ') n--;
      if (n == 0) n = cols;  // one long word
    }
    display.write((const uint8_t*)text, n);
    display.println();
    text += n;
  }
}

void drawAlert(const HubAlert& a, int index, int count) {
  char pos[24];
  snprintf(pos, sizeof pos, "%d/%d", index + 1, count);
  char text[sizeof a.message];

  display.fillRect(0, 0, 128, 9, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setCursor(1, 1);
  display.print(LEVEL_NAMES[a.level]);
  display.setCursor(127 - 6 * strlen(pos), 1);
  display.print(pos);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 12);
  display.setTextWrap(false);
  toDisplayText(text, sizeof text, a.title);
  display.println(text);
  display.setTextWrap(true);
  toDisplayText(text, sizeof text, a.message);
  printWrapped(text, 5);
}

void drawStatus(bool connected, int rssi, int percent, int alertCount) {
  if (connected) {
    display.println("WiFi connected!");
    display.println(WiFi.SSID());
    display.print("MQTT: ");
    display.print(hub.statusText());
    if (alertCount) display.printf(" !%d", alertCount);
    display.println();
    display.print("IP: ");
    display.println(WiFi.localIP());
    display.print("Signal: ");
    display.print(rssi);
    display.print(" dBm ");
    display.print(percent);
    display.println("%");
    drawSignalBar(percent);
  } else {
    display.println("Connecting to WiFi...");
    display.println(ssid);
    display.println();
    display.println("Check name/password");
    display.println("and 2.4 GHz network.");
  }
}

void setup() {
  Serial.begin(115200);

  Wire.begin(D5, D6);
  oledReady = display.begin(
    SSD1306_SWITCHCAPVCC, 0x3C, true, false
  );

  if (oledReady) {
    display.cp437(true);
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("Connecting to WiFi...");
    display.display();
  }

  hub.showAlerts = showAlerts;
  hub.onHealth([](JsonObject h) {
    h["app"] = "nodemcu_disp";
    h["display"] = oledReady ? "SSD1306 128x64" : nullptr;
  });

  WiFi.mode(WIFI_STA);
  hub.begin();
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);

  Serial.println("\nConnecting to WiFi...");
}

void loop() {
  static unsigned long lastDraw = 0, lastSerial = 0;
  hub.loop();

  unsigned long now = millis();
  if (now - lastDraw < 1000) {
    delay(10);
    return;
  }
  lastDraw = now;

  bool connected = WiFi.status() == WL_CONNECTED;
  int rssi = connected ? WiFi.RSSI() : 0;
  int percent = HubMqtt::rssiToPercent(rssi);
  const HubAlert* alerts[HubMqtt::MAX_ALERTS];
  int alertCount = hub.alerts(alerts);

  if (oledReady) {
    display.clearDisplay();
    display.setCursor(0, 0);
    // With alerts, the status page and each alert take turns, 4 s each.
    int page = (now / 4000) % (1 + alertCount);
    if (page == 0) {
      drawStatus(connected, rssi, percent, alertCount);
    } else {
      drawAlert(*alerts[page - 1], page - 1, alertCount);
    }
    display.display();
  }

  if (now - lastSerial < 2000) return;
  lastSerial = now;
  if (connected) {
    Serial.print("IP: ");
    Serial.print(WiFi.localIP());
    Serial.print(" | Signal: ");
    Serial.print(rssi);
    Serial.print(" dBm (");
    Serial.print(percent);
    Serial.print("%) | MQTT: ");
    Serial.print(hub.statusText());
    Serial.print(" | alerts: ");
    Serial.println(alertCount);
  } else {
    Serial.print("Waiting, WiFi status: ");
    Serial.println((int)WiFi.status());
  }
}
