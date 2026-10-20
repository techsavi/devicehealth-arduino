# Arduino

ESP8266 (NodeMCU) sketches that report to a home MQTT hub, plus the shared library they use.

Each device keeps an online/offline status on the broker, publishes a health report every 30 s, and can show the hub's alerts. The hub is a separate project: a Mosquitto broker with a web page of tiles. Any Mosquitto broker with per-device logins works the same way.

## Layout

This folder is an Arduino **sketchbook**: one folder per sketch, and shared code in `libraries/`.

```
nodemcu_disp/            WiFi signal meter with a 128x64 SSD1306 OLED
nodemcu_nodisp/          WiFi signal meter without a display: reports the signal over MQTT
libraries/HubMqtt/       shared MQTT client for the hub (ours, committed)
libraries/<others>/      third-party libraries from the Library Manager (git-ignored)
```

Each sketch has a `secrets.example.h` (committed) and a `secrets.h` (git-ignored) with the real WiFi and MQTT logins.

## Setup

1. **Arduino IDE 2**, with **File → Preferences → Sketchbook location** set to this folder, so the IDE finds `libraries/HubMqtt`.
2. **ESP8266 boards:** add `https://arduino.esp8266.com/stable/package_esp8266com_index.json` under *Additional boards manager URLs*, then install **esp8266** in the Boards Manager. Board: **NodeMCU 1.0 (ESP-12E Module)**.
3. **Libraries** (Library Manager, or arduino-cli):
   ```sh
   arduino-cli lib install PubSubClient ArduinoJson "Adafruit SSD1306" "Adafruit GFX Library"
   ```
   The two Adafruit libraries are only needed for `nodemcu_disp`.
4. **Credentials:** copy the sketch's `secrets.example.h` to `secrets.h` and fill it in:
   - your 2.4 GHz WiFi network and password (the ESP8266 has no 5 GHz)
   - the broker's LAN address
   - the device's MQTT password

   Each device logs in with its own name, which is `mqttName` in the sketch: `nodemcu-disp` or `nodemcu-nodisp`. On the hub, add the device on the IoT Devices page, or run `node serve.mjs mqtt-user <name>`; either one creates the login and its password.
5. Upload, then watch the Serial Monitor at 115200 baud. You should see `MQTT: connected to … as <name>`.

## Sketches

### nodemcu_disp

Shows the WiFi network, IP, signal (dBm and %) and MQTT status, with a 10-segment signal bar. If alerts are on, the status page and each alert take turns on the screen, 4 s each. The hub's **Identify** button fills the screen with the device's name.

Wiring: SSD1306 128x64 over I²C at address `0x3C`, SDA on **D5**, SCL on **D6**.

### nodemcu_nodisp

The same meter without a screen. Every 5 s (`signalIntervalMs`) it publishes a retained `devices/nodemcu-nodisp/wifi` reading, `{dbm, pct, ssid, bssid, channel, at}`, and the hub's IoT Devices tile shows it live as signal bars.

## MQTT topics

| Topic | Direction | Retained | Payload |
| --- | --- | --- | --- |
| `status/<name>` | device → hub | yes | `online`; the broker sets `offline` (the last will) when the device drops |
| `devices/<name>/health` | device → hub | yes | JSON every 30 s: `kind: "nodemcu"`, chip, core/SDK, IP, uptime, heap, reset reason, `wifi {ssid, bssid, channel, dbm, pct}`, plus the sketch's own fields (`app`, `display`, `commands`) |
| `devices/<name>/<reading>` | device → hub | as the sketch chooses | The sketch's own data, e.g. `devices/nodemcu-nodisp/wifi` |
| `alerts/<source>/<id>` | hub → device | yes | `{level, title, message, since, expires?}`; an empty message clears the alert |
| `registry/devices/<name>` | hub → device | yes | The hub's record of the device; `alerts` (`all`, `warning`, `critical`, `off`, or absent) picks which alerts its display shows |
| `cmd/<name>/identify` | hub → device | no | `{seconds, text, subtitle}`: show the name on the display |

## HubMqtt

`libraries/HubMqtt` handles the broker connection for every sketch:
- connecting and reconnecting (every 5 s at first, backing off to once a minute) as WiFi comes and goes
- the online/offline status and the health report
- the clock (UTC, set over NTP), used to date reports and expire alerts
- the alert list, filtered by the hub's per-device setting
- the Identify command

```cpp
#include <HubMqtt.h>
#include "secrets.h"

HubMqtt hub(MQTT_HOST, 1883, "nodemcu-garage", MQTT_PASSWORD);

void setup() {
  hub.showAlerts = false;                                     // default when the hub doesn't say
  hub.onHealth([](JsonObject h) { h["app"] = "garage"; });   // extra health fields
  WiFi.mode(WIFI_STA);
  hub.begin();                                                // before WiFi.begin()
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void loop() {
  hub.loop();   // call often; avoid long delay()s
  // hub.publish("temperature", "21.5");                      // devices/<name>/temperature
  // hub.raiseAlert("door", HUB_WARNING, "Garage", "Door open"); / hub.clearAlert("door");
}
```

Other calls:
- `hub.alerts(list)` returns the alerts to show, most severe first.
- `hub.onIdentify(fn)` handles the hub's Identify button, and also tells the hub that the device supports it.
- `hub.onConnect(fn)` and `hub.onMessage(fn)` let a sketch subscribe to topics of its own.
- `hub.statusText()` returns `online`, `login refused` or `connecting...`.

See [HubMqtt.h](libraries/HubMqtt/src/HubMqtt.h) for the details.

The broker port (1883) is unencrypted, which is normal on a home LAN because the ESP8266 can't afford TLS. Never forward that port from your router.

## Adding a sketch

1. Create a new folder, copy a `secrets.example.h` into it, and pick a unique `mqttName`.
2. Use `HubMqtt` as shown above.
3. Create the device's login on the hub and put the password in `secrets.h`.
4. If the sketch uses a new third-party library, add it to the install command above.
