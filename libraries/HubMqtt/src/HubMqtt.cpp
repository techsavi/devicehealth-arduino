#include "HubMqtt.h"
#include <time.h>

const char* const AGENT_VERSION = "1.1";
const char* const LEVEL_NAMES[] = {"info", "warning", "critical"};
const char* const ALL_LEVELS_FROM[] = {"manual", "tabletpanel"};
// How long to wait for the retained registry entry after connecting before taking it as unset.
const unsigned long REGISTRY_WAIT_MS = 3000;

static uint32_t fnv1a(const char* s) {
  uint32_t h = 2166136261u;
  while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
  return h ? h : 1;
}

// Epoch seconds, or 0 until NTP has set the clock.
static uint32_t clockNow() {
  time_t now = time(nullptr);
  return now > 1700000000 ? (uint32_t)now : 0;
}

// Copy UTF-8 into dst without splitting a character; control characters become spaces.
static void copyUtf8(char* dst, size_t size, const char* src) {
  size_t n = 0;
  while (src[n] && n + 1 < size) n++;
  if (src[n]) {
    while (n > 0 && ((uint8_t)src[n] & 0xC0) == 0x80) n--;  // back to the start of a cut character
  }
  for (size_t i = 0; i < n; i++) dst[i] = (uint8_t)src[i] < 0x20 ? ' ' : src[i];
  dst[n] = 0;
}

HubMqtt::HubMqtt(const char* host, uint16_t port, const char* name, const char* password)
    : mqtt_(net_), host_(host), port_(port), name_(name), password_(password) {}

void HubMqtt::begin() {
  WiFi.hostname(name_);
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  statusTopic_ = String("status/") + name_;
  healthTopic_ = String("devices/") + name_ + "/health";
  registryTopic_ = String("registry/devices/") + name_;
  identifyTopic_ = String("cmd/") + name_ + "/identify";
  alertFilter_["level"] = true;
  alertFilter_["title"] = true;
  alertFilter_["message"] = true;
  alertFilter_["expires"] = true;

  mqtt_.setServer(host_, port_);
  mqtt_.setCallback([this](char* topic, uint8_t* payload, unsigned int length) {
    handleMessage(topic, payload, length);
  });
  mqtt_.setBufferSize(2048);  // incoming messages larger than this are skipped
  // The health reports keep the connection alive; the broker declares this device offline
  // after 1.5 keepalives without traffic.
  mqtt_.setKeepAlive(healthIntervalMs / 1000 * 2);
  mqtt_.setSocketTimeout(5);
}

bool HubMqtt::connect() {
  String clientId = String(name_) + "-esp";
  if (!mqtt_.connect(clientId.c_str(), name_, password_, statusTopic_.c_str(), 0, true, "offline")) {
    Serial.printf("MQTT: connecting to %s:%u failed (state %d)\n", host_, port_, mqtt_.state());
    return false;
  }
  memset(alerts_, 0, sizeof alerts_);  // the active ones arrive again (retained) on subscribing
  alertsSubscribed_ = false;
  mqtt_.publish(statusTopic_.c_str(), "online", true);
  // alerts/# waits for the registry entry (retained, so it comes at once if there is one), so a
  // device set to "off" doesn't flash the alerts.
  mqtt_.subscribe(registryTopic_.c_str());
  registryPending_ = true;
  registryDeadline_ = millis() + REGISTRY_WAIT_MS;
  if (onIdentify_) mqtt_.subscribe(identifyTopic_.c_str());
  if (onConnect_) onConnect_();
  lastHealth_ = millis() - healthIntervalMs;  // report at once
  Serial.printf("MQTT: connected to %s:%u as %s\n", host_, port_, name_);
  return true;
}

// Keeps the broker connection up (retrying every 5 s, backing off to a minute) and reports health.
void HubMqtt::loop() {
  bool wifiUp = WiFi.status() == WL_CONNECTED;
  if (wifiUp != wifiUp_) {
    wifiUp_ = wifiUp;
    if (wifiUp) {
      nextTry_ = millis();  // try the broker now
      backoff_ = 5000;
    } else {
      net_.stop();  // the broker will have dropped it; start fresh when WiFi is back
    }
  }
  if (!wifiUp) return;

  if (!mqtt_.connected()) {
    if ((long)(millis() - nextTry_) < 0) return;
    if (!connect()) {
      nextTry_ = millis() + backoff_;
      backoff_ = min(backoff_ * 2, 60000UL);
      return;
    }
    backoff_ = 5000;
  }
  mqtt_.loop();
  if (registryPending_ && (long)(millis() - registryDeadline_) >= 0) {
    registryPending_ = false;  // no registry entry
    alertsSetting_ = ALERTS_DEFAULT;
    applyAlertsSetting();
  }
  if (millis() - lastHealth_ >= healthIntervalMs) {
    lastHealth_ = millis();
    publishHealth();
  }
}

bool HubMqtt::connected() { return mqtt_.connected(); }

const char* HubMqtt::statusText() {
  if (mqtt_.connected()) return "online";
  switch (mqtt_.state()) {
    case MQTT_CONNECT_BAD_CREDENTIALS:
    case MQTT_CONNECT_UNAUTHORIZED: return "login refused";
    default: return "connecting...";
  }
}

void HubMqtt::publishHealth() {
  JsonDocument h;
  h["kind"] = kind;
  h["agent"] = AGENT_VERSION;
  if (uint32_t now = clockNow()) h["at"] = now;
  h["hostname"] = WiFi.hostname();
  h["model"] = model;
  h["chip_id"] = String(ESP.getChipId(), HEX);
  h["core"] = ESP.getCoreVersion();
  h["sdk"] = ESP.getSdkVersion();
  h["ip"] = WiFi.localIP().toString();
  h["uptime_s"] = (uint32_t)(micros64() / 1000000);
  h["reset_reason"] = ESP.getResetReason();
  h["heap_free"] = ESP.getFreeHeap();
  h["heap_frag_pct"] = ESP.getHeapFragmentation();
  h["cpu_mhz"] = ESP.getCpuFreqMHz();
  h["flash_size"] = ESP.getFlashChipRealSize();
  h["sketch_size"] = ESP.getSketchSize();
  JsonObject w = h["wifi"].to<JsonObject>();
  int rssi = WiFi.RSSI();
  w["ssid"] = WiFi.SSID();
  w["bssid"] = WiFi.BSSIDstr();
  w["channel"] = WiFi.channel();
  w["dbm"] = rssi;
  w["pct"] = rssiToPercent(rssi);
  if (onIdentify_) h["commands"].add("identify");
  if (onHealth_) onHealth_(h.as<JsonObject>());

  mqtt_.beginPublish(healthTopic_.c_str(), measureJson(h), true);
  serializeJson(h, mqtt_);
  mqtt_.endPublish();
}

bool HubMqtt::publish(const char* subtopic, const char* payload, bool retain) {
  String topic = String("devices/") + name_ + "/" + subtopic;
  return mqtt_.publish(topic.c_str(), payload, retain);
}

bool HubMqtt::raiseAlert(const char* id, HubAlertLevel level, const char* title, const char* message) {
  JsonDocument a;
  a["level"] = levelName(level);
  a["title"] = title;
  a["message"] = message;
  if (uint32_t now = clockNow()) a["since"] = now;
  String topic = String("alerts/") + name_ + "/" + id;
  mqtt_.beginPublish(topic.c_str(), measureJson(a), true);
  serializeJson(a, mqtt_);
  return mqtt_.endPublish();
}

bool HubMqtt::clearAlert(const char* id) {
  String topic = String("alerts/") + name_ + "/" + id;
  return mqtt_.publish(topic.c_str(), "", true);
}

void HubMqtt::handleMessage(char* topic, uint8_t* payload, unsigned int length) {
  if (registryTopic_ == topic) {
    handleRegistry(payload, length);
  } else if (identifyTopic_ == topic) {
    handleIdentify(payload, length);
  } else if (!strncmp(topic, "alerts/", 7)) {
    handleAlert(topic, payload, length);
  } else if (onMessage_) {
    onMessage_(topic, payload, length);
  }
}

// registry/devices/<name>: the hub's record of this device; "alerts" is its Show alerts setting.
void HubMqtt::handleRegistry(const uint8_t* payload, unsigned int length) {
  JsonDocument filter;
  filter["alerts"] = true;
  JsonDocument doc;
  if (length) deserializeJson(doc, (const char*)payload, length, DeserializationOption::Filter(filter));
  const char* value = doc["alerts"] | "";
  alertsSetting_ = !strcmp(value, "all")        ? ALERTS_ALL
                   : !strcmp(value, "warning")  ? ALERTS_WARNING
                   : !strcmp(value, "critical") ? ALERTS_CRITICAL
                   : !strcmp(value, "off")      ? ALERTS_OFF
                                                : ALERTS_DEFAULT;
  registryPending_ = false;
  applyAlertsSetting();
}

// Subscribes to alerts/# or leaves it as the setting needs. The alerts already received are kept
// whatever their level and filtered by alerts(), so a changed level shows at the next redraw.
void HubMqtt::applyAlertsSetting() {
  bool want = wantsAlerts();
  if (want && !alertsSubscribed_) {
    alertsSubscribed_ = mqtt_.subscribe("alerts/#");
  } else if (!want && alertsSubscribed_) {
    mqtt_.unsubscribe("alerts/#");
    alertsSubscribed_ = false;
    memset(alerts_, 0, sizeof alerts_);
  }
}

bool HubMqtt::wantsAlerts() {
  return alertsSetting_ == ALERTS_DEFAULT ? showAlerts : alertsSetting_ != ALERTS_OFF;
}

bool HubMqtt::shows(const HubAlert& a) {
  switch (alertsSetting_) {
    case ALERTS_ALL: return true;
    case ALERTS_WARNING: return a.level >= HUB_WARNING;
    case ALERTS_CRITICAL: return a.level >= HUB_CRITICAL;
    case ALERTS_OFF: return false;
    default: return showAlerts && (a.level >= HUB_WARNING || a.allLevels);
  }
}

// cmd/<name>/identify {seconds, text, subtitle}, from the hub's Identify button (not retained).
void HubMqtt::handleIdentify(const uint8_t* payload, unsigned int length) {
  JsonDocument doc;
  if (length) deserializeJson(doc, (const char*)payload, length);
  unsigned long seconds = doc["seconds"] | 30;
  const char* text = doc["text"] | "";
  onIdentify_(min(seconds, 600UL), *text ? text : name_, doc["subtitle"] | "");
}

void HubMqtt::handleAlert(const char* topic, const uint8_t* payload, unsigned int length) {
  uint32_t hash = fnv1a(topic);
  int slot = -1;
  for (int i = 0; i < MAX_ALERTS; i++) {
    if (alerts_[i].topicHash == hash) slot = i;
  }

  JsonDocument doc;
  bool ok = length > 0 &&
            !deserializeJson(doc, (const char*)payload, length, DeserializationOption::Filter(alertFilter_));
  if (!ok) {
    if (slot >= 0) alerts_[slot].topicHash = 0;  // cleared
    return;
  }
  const char* lvl = doc["level"] | "info";
  uint8_t level = !strcmp(lvl, "critical") ? HUB_CRITICAL : !strcmp(lvl, "warning") ? HUB_WARNING : HUB_INFO;

  // alerts/<source>/<id>
  const char* src = topic + 7;
  const char* end = strchr(src, '/');
  size_t srcLen = end ? end - src : strlen(src);
  bool allLevels = strlen(name_) == srcLen && !strncmp(src, name_, srcLen);
  for (const char* s : ALL_LEVELS_FROM) {
    if (strlen(s) == srcLen && !strncmp(src, s, srcLen)) allLevels = true;
  }

  if (slot < 0) slot = freeSlot(level);
  if (slot < 0) return;  // full of more severe alerts

  HubAlert& a = alerts_[slot];
  a.topicHash = hash;
  a.level = level;
  a.allLevels = allLevels;
  a.expires = doc["expires"] | 0;
  const char* title = doc["title"] | "";
  if (*title) {
    copyUtf8(a.title, sizeof a.title, title);
  } else {
    snprintf(a.title, sizeof a.title, "%.*s", (int)srcLen, src);
  }
  copyUtf8(a.message, sizeof a.message, doc["message"] | "");
}

// A free slot, else the least severe alert below `level` (bumped for a more severe one), else -1.
int HubMqtt::freeSlot(uint8_t level) {
  int lowest = -1;
  for (int i = 0; i < MAX_ALERTS; i++) {
    if (!alerts_[i].topicHash) return i;
    if (alerts_[i].level < level && (lowest < 0 || alerts_[i].level < alerts_[lowest].level)) lowest = i;
  }
  return lowest;
}

int HubMqtt::alerts(const HubAlert** out) {
  uint32_t now = clockNow();
  int n = 0;
  for (int i = 0; i < MAX_ALERTS; i++) {
    HubAlert& a = alerts_[i];
    if (!a.topicHash) continue;
    if (a.expires && now && now >= a.expires) {
      a.topicHash = 0;
      continue;
    }
    if (!shows(a)) continue;
    int j = n++;
    while (j > 0 && out[j - 1]->level < a.level) {
      out[j] = out[j - 1];
      j--;
    }
    out[j] = &a;
  }
  return n;
}

const char* HubMqtt::levelName(uint8_t level) {
  return LEVEL_NAMES[level <= HUB_CRITICAL ? level : (uint8_t)HUB_INFO];
}

int HubMqtt::rssiToPercent(int rssi) {
  if (rssi <= -100) return 0;
  if (rssi >= -50) return 100;
  return 2 * (rssi + 100);
}
