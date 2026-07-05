# ColdGuard Edge

Firmware for an ESP32-based cold storage monitor designed to protect amoxicillin stock. The device connects to a cloud backend and an MQTT broker, allowing an AI agent to read sensor data and control actuators in real time. Complex automation logic can be pushed to the device as Lua scripts over MQTT — no reflash needed.

---

## How It Works

```
AI Agent / Dashboard
       │
       ▼
  MQTT Broker  ──────────────────────────────────────┐
  coldGuard/command                                   │
       │                                              │
       ▼                                          responses
    ESP32                                             │
  ┌─────────────────────────────────────┐             │
  │  ASHA Bridge                        │─────────────┘
  │  • Parses JSON commands             │  coldGuard/response
  │  • Runs embedded Lua VM            │  asha/response/<device_id>
  │  • Reads DHT11 (temp/humidity)     │
  │  • Controls pins / PWM             │
  └─────────────────────────────────────┘
       │
       ▼
  FastAPI Backend (Alibaba Cloud VPS)
  POST /api/v1/sensor/add_data   ← every 2 minutes
  POST /api/v1/asha/verify_and_register_device  ← on boot
```

---

## Hardware

| Component | Pin | Type |
|-----------|-----|------|
| Red Light | 18 | Digital Actuator |
| Green Light | 19 | Digital Actuator |
| Buzzer | 21 | PWM Actuator |
| DHT11 (Temp/Humidity) | 22 | Sensor |
| Fan | 23 | Digital Actuator |

The device supports up to **30 registered devices**. Add or remove devices in `main.cpp` before flashing.

---

## Project Structure

```
coldGuardEdge/
├── src/
│   ├── main.cpp          # Device setup — register devices, init ASHA
│   └── config.h          # All configurable values (WiFi, MQTT, URLs)
├── lib/
│   └── ASHABridge/
│       ├── ASHA.h        # Class definitions and enums
│       └── ASHA.cpp      # Core runtime: WiFi, MQTT, Lua, HTTP
└── platformio.ini        # Build config and library dependencies
```

---

## Configuration

All values that need to change between environments live in [`src/config.h`](src/config.h):

```c
#define SENSOR_API_URL     "http://<vps-ip>:<port>/api/v1/sensor/add_data"
#define ASHA_REGISTER_URL  "http://<vps-ip>:<port>/api/v1/asha/verify_and_register_device"
#define WIFI_SSID          "your-wifi-name"
#define WIFI_PASSWORD      "your-wifi-password"
#define MQTT_BROKER        "192.168.x.x"      // IP of your MQTT broker
#define MQTT_PORT          1883
#define ASHA_ID            "your-device-uuid"
```

> **Note:** `main.cpp` also has `ssid` and `password` constants at the top — these should match `WIFI_SSID` / `WIFI_PASSWORD` in `config.h`. Consider consolidating them.

---

## Getting Started

### Prerequisites

- [PlatformIO](https://platformio.org/) (VS Code extension or CLI)
- An MQTT broker reachable from the ESP32 (e.g. Mosquitto on a local machine or the same VPS)
- The [ASHA backend](https://github.com/) running and accessible

### Flash the Device

1. Clone this repo
2. Open in VS Code with the PlatformIO extension
3. Fill in `src/config.h` with your actual values
4. Set your device UUID in `main.cpp` → `asha.init("your-uuid-here")`
5. Connect the ESP32 via USB and run **Upload** in PlatformIO

### Monitor Serial Output

```
pio device monitor --baud 115200
```

On successful boot you should see:
```
Connecting...
Connected to WiFi
--- Generated ASHA Payload ---
{ ... }
------------------------------
ASHA: Setting up MQTT connection ...
connected to MQTT Broker!
```

---

## MQTT Command Protocol

Publish JSON to the topic **`coldGuard/command`**.

### Digital pin write
```json
{ "action": "digital", "pin": 18, "value": 1 }
```

### Digital pin read
```json
{ "action": "digital", "pin": 18, "value": -1, "correlation_id": "abc123" }
```

### PWM
```json
{ "action": "pwm", "pin": 21, "channel": 0, "freq": 1000, "duty": 4096 }
```

### Analog read
```json
{ "action": "analog", "pin": 34, "correlation_id": "abc123" }
```

### DHT read
```json
{ "action": "DHT", "pin": 22, "correlation_id": "abc123" }
```

### Batch (multiple commands with optional delay)
```json
{
  "action": "batch",
  "commands": [
    { "action": "digital", "pin": 18, "value": 1 },
    { "delay_ms": 500 },
    { "action": "digital", "pin": 18, "value": 0 }
  ]
}
```

Responses (for commands with a `correlation_id`) are published to:
- `coldGuard/response` — general responses
- `asha/response/<device_id>` — device-specific reads

---

## Lua Scripting

The device runs an embedded Lua VM on Core 0. Send a Lua script over MQTT to implement automation logic without reflashing. Any running script is cancelled before the new one starts.

```json
{ "action": "lua", "script": "while true do\n  local temp = asha.getTemperature()\n  if temp > 25 then\n    asha.command('{\"pin\": 23, \"action\": \"digital\", \"value\": 1}')\n  else\n    asha.command('{\"pin\": 23, \"action\": \"digital\", \"value\": 0}')\n  end\n  asha.sleep(500)\nend" }
```

### Available Lua API (`asha.*`)

| Function | Description |
|----------|-------------|
| `asha.command(json_string)` | Execute any ASHA command |
| `asha.getTemperature()` | Returns last cached temperature (°C), or -999 if no reading |
| `asha.getHumidity()` | Returns last cached humidity (%), or -999 if no reading |
| `asha.sleep(ms)` | Yield for `ms` milliseconds (stops cleanly when a new script arrives) |
| `asha.digitalRead(pin)` | Read a digital pin |
| `asha.analogRead(pin)` | Read an analog pin |
| `asha.ledcRead(channel)` | Read PWM duty on a channel |
| `asha.subscribe(topic)` | Subscribe to an MQTT topic |
| `asha.readMessage(topic)` | Get the last payload received on a subscribed topic |

---

## Monitoring MQTT Traffic

With Mosquitto client tools installed:

```bash
# Subscribe to all coldGuard topics
mosquitto_sub -h <MQTT_BROKER_IP> -p 1883 -t "coldGuard/#"

# Publish a test command
mosquitto_pub -h <MQTT_BROKER_IP> -p 1883 -t "coldGuard/command" \
  -m '{"action":"digital","pin":19,"value":1}'
```

On Windows (Mosquitto installed to default path):
```powershell
& "C:\Program Files\Mosquitto\mosquitto_sub.exe" -h <MQTT_BROKER_IP> -p 1883 -t "coldGuard/#"
```

---

## Timings

| Behaviour | Interval |
|-----------|----------|
| DHT11 polling | every 2 seconds |
| Sensor data POST to backend | every 2 minutes |
| MQTT reconnect throttle | 5 seconds between attempts |
| MQTT watchdog (force reconnect if idle) | 30 minutes |

---

## Dependencies

Managed by PlatformIO (`platformio.ini`):

| Library | Purpose |
|---------|---------|
| `bblanchon/ArduinoJson ^7.2.2` | JSON parsing and serialisation |
| `knolleary/PubSubClient ^2.8` | MQTT client |
| `fischer-simon/Esp32Lua ^5.4.7` | Embedded Lua 5.4 VM |
| `adafruit/DHT sensor library` | DHT11/DHT22 driver |
| `adafruit/Adafruit Unified Sensor` | Sensor abstraction layer |
