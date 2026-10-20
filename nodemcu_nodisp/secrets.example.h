// Copy to secrets.h (not in git) and fill in.
#pragma once

#define WIFI_SSID "your-2.4GHz-network"
#define WIFI_PASSWORD "your-wifi-password"

// The webserver PC's address, and this device's login from .run/mqtt/users/nodemcu-nodisp.json
// (created with `node serve.mjs mqtt-user nodemcu-nodisp`).
#define MQTT_HOST "192.168.x.x"
#define MQTT_PASSWORD "mqtt-password"
