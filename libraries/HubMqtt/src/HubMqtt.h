#pragma once
// Connects an ESP8266 to the webserver's MQTT broker (webserver/README.md "MQTT"), like
// webserver/agents/pi/pi_health.py does for a Pi:
//   - status/<name>: "online" while connected, "offline" (the broker's last will) when it drops
//   - devices/<name>/health: a retained JSON report every healthIntervalMs ("kind": "nodemcu")
//   - alerts/#: the active alerts, for the sketch to display, as the hub's per-device "Show alerts"
//     setting (registry/devices/<name>) says, else as showAlerts says
//   - cmd/<name>/identify, when the sketch handles it with onIdentify()
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
  bool allLevels;      // from manual, tabletpanel or this device: shown at every level by default
  uint32_t expires;    // epoch seconds, 0 = until cleared
  char title[32];      // UTF-8, truncated
  char message[128];
};

class HubMqtt {
 public:
  static const int MAX_ALERTS = 8;
  using MessageCallback = std::function<void(const char* topic, const uint8_t* payload, unsigned int length)>;
  using IdentifyCallback = std::function<void(unsigned long seconds, const char* text, const char* subtitle)>;

  HubMqtt(const char* host, uint16_t port, const char* name, const char* password);

  // Settings; change them before begin().
  unsigned long healthIntervalMs = 30000;
  // The device's own default for alerts, used while its registry entry leaves "alerts" unset:
  // warnings and criticals from every source, and every level from manual, tabletpanel and itself.
  bool showAlerts = false;
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
  void onMessage(MessageCallback fn) { onMessage_ = fn; }                 // messages for those topics
  // The hub's Identify button: show `text` (the device's name) large for `seconds`. Setting this
  // also reports "commands": ["identify"] in the health, which enables the button.
  void onIdentify(IdentifyCallback fn) { onIdentify_ = fn; }

  // devices/<name>/<subtopic>
  bool publish(const char* subtopic, const char* payload, bool retain = false);
  // Retained alerts/<name>/<id> (id: letters, digits, - and _). Raise when the condition starts or
  // changes, not on every loop; clearAlert() removes it.
  bool raiseAlert(const char* id, HubAlertLevel level, const char* title, const char* message);
  bool clearAlert(const char* id);

  // The alerts to show under the current setting, most severe first, into out[MAX_ALERTS];
  // returns how many.
  int alerts(const HubAlert** out);

  static const char* levelName(uint8_t level);  // "info", "warning", "critical"
  static int rssiToPercent(int rssi);           // -100 dBm or weaker = 0%, -50 dBm or stronger = 100%

 private:
  // The registry's "alerts": "all", "warning", "critical", "off", or unset (DEFAULT).
  enum AlertsSetting : uint8_t { ALERTS_DEFAULT, ALERTS_ALL, ALERTS_WARNING, ALERTS_CRITICAL, ALERTS_OFF };

  bool connect();
  void publishHealth();
  void handleMessage(char* topic, uint8_t* payload, unsigned int length);
  void handleRegistry(const uint8_t* payload, unsigned int length);
  void handleIdentify(const uint8_t* payload, unsigned int length);
  void handleAlert(const char* topic, const uint8_t* payload, unsigned int length);
  void applyAlertsSetting();
  bool wantsAlerts();
  bool shows(const HubAlert& a);
  int freeSlot(uint8_t level);

  WiFiClient net_;
  PubSubClient mqtt_;
  const char* host_;
  uint16_t port_;
  const char* name_;
  const char* password_;
  String statusTopic_, healthTopic_, registryTopic_, identifyTopic_;
  unsigned long lastHealth_ = 0, nextTry_ = 0, backoff_ = 5000;
  bool wifiUp_ = false;
  AlertsSetting alertsSetting_ = ALERTS_DEFAULT;
  bool registryPending_ = false, alertsSubscribed_ = false;
  unsigned long registryDeadline_ = 0;
  HubAlert alerts_[MAX_ALERTS] = {};
  JsonDocument alertFilter_;
  std::function<void(JsonObject)> onHealth_;
  std::function<void()> onConnect_;
  MessageCallback onMessage_;
  IdentifyCallback onIdentify_;
};
