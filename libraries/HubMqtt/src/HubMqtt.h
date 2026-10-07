#pragma once
// Connects an ESP8266 to the webserver's MQTT broker (webserver/README.md "MQTT"), like
// webserver/agents/pi/pi_health.py does for a Pi:
//   - status/<name>: "online" while connected, "offline" (the broker's last will) when it drops
//   - devices/<name>/health: a retained JSON report every healthIntervalMs ("kind": "nodemcu")
//   - alerts/#, when showAlerts is set: the active alerts, for the sketch to display
// The login is created on the webserver PC with `node serve.mjs mqtt-user <name>`; its password
// is in .run/mqtt/users/<name>.json.
//
//   HubMqtt hub("192.168.1.10", 1883, "nodemcu-x", "password");
//   setup():  WiFi.mode(WIFI_STA); hub.begin(); WiFi.begin(ssid, password);
//   loop():   hub.loop();   // often: it keeps the connection up and reports health
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <functional>

enum HubAlertLevel : uint8_t { HUB_INFO, HUB_WARNING, HUB_CRITICAL };

struct HubAlert {
  uint32_t topicHash;  // 0 = free slot
  uint8_t level;       // HubAlertLevel
  uint32_t expires;    // epoch seconds, 0 = until cleared
  char title[32];      // UTF-8, truncated
  char message[128];
};

class HubMqtt {
 public:
  static const int MAX_ALERTS = 8;
  using MessageCallback = std::function<void(const char* topic, const uint8_t* payload, unsigned int length)>;

  HubMqtt(const char* host, uint16_t port, const char* name, const char* password);

  // Settings; change them before begin().
  unsigned long healthIntervalMs = 30000;
  bool showAlerts = false;  // subscribe to alerts/#
  const char* kind = "nodemcu";
  const char* model = "NodeMCU (ESP8266)";

  // After WiFi.mode() and before WiFi.begin(): sets the hostname to the name and starts NTP (UTC),
  // which dates the reports and expires alerts.
  void begin();
  // Call from loop() without long delays: follows WiFi, (re)connects, reports health.
  void loop();

  bool connected();
  const char* statusText();  // "online", "login refused" or "connecting..."
  PubSubClient& client() { return mqtt_; }

  void onHealth(std::function<void(JsonObject)> fn) { onHealth_ = fn; }  // add fields to each report
  void onConnect(std::function<void()> fn) { onConnect_ = fn; }          // subscribe to more topics here
  void onMessage(MessageCallback fn) { onMessage_ = fn; }                 // messages other than alerts

  // devices/<name>/<subtopic>
  bool publish(const char* subtopic, const char* payload, bool retain = false);
  // Retained alerts/<name>/<id> (id: letters, digits, - and _). Raise when the condition starts or
  // changes, not on every loop; clearAlert() removes it.
  bool raiseAlert(const char* id, HubAlertLevel level, const char* title, const char* message);
  bool clearAlert(const char* id);

  // The alerts to show, most severe first, into out[MAX_ALERTS]; returns how many. Like pi_display:
  // warnings and criticals from every source, every level from manual, tabletpanel and this device.
  int alerts(const HubAlert** out);

  static const char* levelName(uint8_t level);  // "info", "warning", "critical"
  static int rssiToPercent(int rssi);           // -100 dBm or weaker = 0%, -50 dBm or stronger = 100%

 private:
  bool connect();
  void publishHealth();
  void handleMessage(char* topic, uint8_t* payload, unsigned int length);
  void handleAlert(const char* topic, const uint8_t* payload, unsigned int length);
  int freeSlot(uint8_t level);

  WiFiClient net_;
  PubSubClient mqtt_;
  const char* host_;
  uint16_t port_;
  const char* name_;
  const char* password_;
  String statusTopic_, healthTopic_;
  unsigned long lastHealth_ = 0, nextTry_ = 0, backoff_ = 5000;
  bool wifiUp_ = false;
  HubAlert alerts_[MAX_ALERTS] = {};
  JsonDocument alertFilter_;
  std::function<void(JsonObject)> onHealth_;
  std::function<void()> onConnect_;
  MessageCallback onMessage_;
};
