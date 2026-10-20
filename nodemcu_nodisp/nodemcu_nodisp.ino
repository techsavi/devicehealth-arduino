// WiFi signal meter without a display: reports the signal over MQTT for the hub's IoT Devices tile.
#include <ESP8266WiFi.h>
#include <HubMqtt.h>
#include <time.h>
#include "secrets.h"  // WiFi and MQTT logins: copy secrets.example.h

const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// MQTT broker on the webserver PC (webserver/README.md "MQTT").
const char* mqttHost = MQTT_HOST;
const uint16_t mqttPort = 1883;
const char* mqttName = "nodemcu-nodisp";
const char* mqttPassword = MQTT_PASSWORD;

// devices/<name>/wifi (retained): {dbm, pct, ssid, bssid, channel, at} this often.
const unsigned long signalIntervalMs = 5000;

HubMqtt hub(mqttHost, mqttPort, mqttName, mqttPassword);

void publishSignal() {
  int rssi = WiFi.RSSI();
  JsonDocument s;
  s["dbm"] = rssi;
  s["pct"] = HubMqtt::rssiToPercent(rssi);
  s["ssid"] = WiFi.SSID();
  s["bssid"] = WiFi.BSSIDstr();
  s["channel"] = WiFi.channel();
  time_t now = time(nullptr);
  if (now > 1700000000) s["at"] = (uint32_t)now;  // once NTP has set the clock

  char buf[160];
  serializeJson(s, buf, sizeof buf);
  hub.publish("wifi", buf, true);

  Serial.printf("Signal: %d dBm (%d%%)\n", rssi, HubMqtt::rssiToPercent(rssi));
}

void setup() {
  Serial.begin(115200);

  hub.onHealth([](JsonObject h) {
    h["app"] = "nodemcu_nodisp";
    h["display"] = nullptr;
  });

  WiFi.mode(WIFI_STA);
  hub.begin();
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);

  Serial.println("\nConnecting to WiFi...");
}

void loop() {
  static unsigned long lastSignal = 0, lastWait = 0;
  hub.loop();

  unsigned long now = millis();
  if (hub.connected()) {
    if (now - lastSignal >= signalIntervalMs) {
      lastSignal = now;
      publishSignal();
    }
  } else if (now - lastWait >= 2000) {
    lastWait = now;
    Serial.printf("Waiting, WiFi status: %d, MQTT: %s\n", (int)WiFi.status(), hub.statusText());
  }
  delay(10);
}
