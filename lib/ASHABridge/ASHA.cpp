#include "ASHA.h"
#include "config.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>

#include <Lua.h>
#include <WiFi.h>
#include <Wire.h>

#include <lua/lua.hpp>

ASHA *ASHA::instance = nullptr;

// * ============================================================
// * LUA API — functions exposed to Lua scripts via asha.*
// * ============================================================

static int lua_ashaCommand(lua_State *L)
{
    const char *raw_command = luaL_checkstring(L, 1);
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, raw_command);
    // TODO: the error here fails silenty, print it out so that we know whats wrong
    if (!error)
    {
        String action = doc["action"];

        if (action == "batch")
        {
            JsonArray commands = doc["commands"];
            for (JsonVariant cmd : commands)
            {
                ASHA::handleCommand(cmd);
            }
        }
        else
        {
            ASHA::handleCommand(doc.as<JsonVariant>());
        }
    }
    return 0;
}

static int lua_digitalWrite(lua_State *L)
{
    int pin = luaL_checkinteger(L, 1);
    int value = luaL_checkinteger(L, 2);
    digitalWrite(pin, value);
    return 0;
}

int lua_digitalRead(lua_State *L)
{
    int pin = luaL_checkinteger(L, 1);
    int val = digitalRead(pin);
    lua_pushinteger(L, val);
    return 1;
}

int lua_delay(lua_State *L)
{
    int ms = luaL_checkinteger(L, 1);
    delay(ms);
    return 0;
}

int lua_analogRead(lua_State *L)
{
    int pin = luaL_checkinteger(L, 1);
    int val = analogRead(pin);
    lua_pushinteger(L, val);
    return 1;
}

int lua_ledcRead(lua_State *L)
{
    int channel = luaL_checkinteger(L, 1);
    int duty = ledcRead(channel);
    lua_pushinteger(L, duty);
    return 1;
}

int lua_sleep(lua_State *L)
{
    int ms = luaL_checkinteger(L, 1);
    vTaskDelay(pdMS_TO_TICKS(ms));
    if (ASHA::getInstance()->getStopLuaRequested())
    {
        luaL_error(L, "asha agent stopped current script");
    }
    return 0;
}

int lua_subscribe(lua_State *L)
{
    const char *topic = luaL_checkstring(L, 1);
    ASHA::getInstance()->subscribeTopic(topic);
    return 0;
}

int lua_getTemperature(lua_State *L)
{
    float temp = ASHA::getInstance()->cachedTemp;
    lua_pushnumber(L, isnan(temp) ? -999 : temp);
    return 1;
}

int lua_getHumidity(lua_State *L)
{
    float hum = ASHA::getInstance()->cachedHumidity;
    lua_pushnumber(L, isnan(hum) ? -999 : hum);
    return 1;
}

int lua_getVar(lua_State *L)
{
    const char *topic = luaL_checkstring(L, 1);
    const std::string message = ASHA::getInstance()->gettopicPayload(topic);
    lua_pushstring(L, message.c_str());
    return 1;
}

static const luaL_Reg ashalib[] = {{"command", lua_ashaCommand},
                                   {"analogRead", lua_analogRead},
                                   {"digitalRead", lua_digitalRead},
                                   {"ledcRead", lua_ledcRead},
                                   {"sleep", lua_sleep},
                                   {"subscribe", lua_subscribe},
                                   {"readMessage", lua_getVar},
                                   {"getTemperature", lua_getTemperature},
                                   {"getHumidity", lua_getHumidity},
                                   {nullptr, nullptr}};

int luaopen_asha(lua_State *L)
{
    luaL_newlib(L, ashalib);
    return 1;
}

// * ============================================================
// * MQTT SUBSCRIPTION — queued from Lua (Core 0), drained by run() on Core 1
// * ============================================================
void ASHA::subscribeTopic(const char *topic)
{
    char *copy = strdup(topic);
    xQueueSend(pendingSubscriptions, &copy, 0);
}

// * ============================================================
// * WIFI
// * ============================================================
void ASHA_WIFI::onConnect(void (*userdefinedfunc)())
{
    userdefinedfunLoc = userdefinedfunc;
}

void ASHA_WIFI::begin(const char *ssid, const char *password)
{
    WiFi.mode(WIFI_STA);
    IPAddress primaryDNS(8, 8, 8, 8);
    IPAddress secondaryDNS(8, 8, 4, 4);
    WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, primaryDNS, secondaryDNS);

    WiFi.begin(ssid, password);
    Serial.begin(115200);
    Serial.print("Connecting");
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nConnected to WiFi");

    if (userdefinedfunLoc != nullptr)
    {
        userdefinedfunLoc();
    }
}

// * ============================================================
// * DEVICES — registry and actuator factory
// * ============================================================
int ASHA_Devices::addDevice(const DeviceType &deviceType, int pin)
{
    if (count >= MAX_DEVICES)
    {
        return -1; // this flag signifies that its full
    }

    int newId = count + 1;
    devices[count] = {newId, deviceType, pin};
    count++;

    return newId;
}

DeviceType ASHA::genericDev(DeviceCategory deviceCategory, const std::string &metadata,
                            BusType busType)
{
    DeviceType dt;
    dt.category = deviceCategory;
    dt.metadata = metadata;
    dt.bus = busType;
    return dt;
}

// * ============================================================
// * INIT — register devices with cloud, connect MQTT, start Lua task
// * ============================================================
std::string ASHA::init(const std::string &ashaID)
{
    instance = this;
    JsonDocument doc;

    doc["auth_id"] = ashaID;
    JsonArray jsonDevices = doc["devices"].to<JsonArray>();

    bool i2cInitialized = false;
    bool spiInitialized = false;
    for (int i = 0; i < asha_devices.getCount(); ++i)
    {
        RegisteredDevice rd = asha_devices.getDevice(i);

        JsonVariant deviceObj = jsonDevices.add<JsonVariant>();
        deviceObj["device_id"] = rd.id;
        deviceObj["pin"] = rd.pin;

        if (rd.deviceType.category == DeviceCategory::Actuator)
        {
            deviceObj["category"] = "Actuator";
        }
        else
        {
            deviceObj["category"] = "Sensor";
        }

        deviceObj["metadata"] = rd.deviceType.metadata;

        // 2. Convert Bus to String
        switch (rd.deviceType.bus)
        {
        case BusType::Digital:
            deviceObj["bus"] = "Digital";
            break;
        case BusType::Analog:
            deviceObj["bus"] = "Analog";
            break;
        case BusType::pwm:
            deviceObj["bus"] = "PWM";
            break;
        case BusType::DHT11:
            deviceObj["bus"] = "DHT11";
            break;
        case BusType::DHT22:
            deviceObj["bus"] = "DHT22";
            break;
        }

        // hardware init
        if (rd.deviceType.bus == BusType::Digital)
        {
            if (rd.deviceType.category == DeviceCategory::Actuator)
            {
                pinMode(rd.pin, OUTPUT);
            }
            else
            {
                pinMode(rd.pin, INPUT);
            }
        }
        else if (rd.deviceType.bus == BusType::DHT11 || rd.deviceType.bus == BusType::DHT22)
        {
            uint8_t dhtType = (rd.deviceType.bus == BusType::DHT11) ? DHT11 : DHT22;
            DHT *dht = new DHT(rd.pin, dhtType);
            dht->begin();
            dhtSensors[rd.pin] = dht;
        }
    }

    // sending payload
    std::string payload;
    serializeJson(doc, payload);

    Serial.println("--- Generated ASHA Payload ---");
    Serial.println(payload.c_str());
    Serial.println("------------------------------");

    if (WiFi.status() == WL_CONNECTED)
    {
        HTTPClient http;
        WiFiClient client;

        Serial.println("ASHA: Payload ready to be sent to the cloud.");
        http.begin(client, ASHA_REGISTER_URL);

        http.addHeader("Content-Type", "application/json");
        http.addHeader("ngrok-skip-browser-warning", "69420");

        int httpResponseCode = http.POST(String(payload.c_str()));

        if (httpResponseCode > 0)
        {
            Serial.print("HTTP Response code: ");
            Serial.println(httpResponseCode);
            String response = http.getString();
            Serial.println(response);
        }
        else
        {
            Serial.print("Error sending request. Error code: ");
            Serial.println(httpResponseCode);
        }

        http.end();
    }
    else
    {
        Serial.println("ASHA: Not connected to WiFi. Cannot send payload.");
    }
    Serial.println("ASHA: Setting up MQTT connection ...");

    mqttClient.setClient(espClient);
    mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
    mqttClient.setBufferSize(16 * 1024);
    mqttClient.setCallback(mqttCallback);
    currentAshaID = ashaID;

    // LUA -------------------------------------------------------------------------------
    ashaLua.addModule("asha", luaopen_asha);
    luaScriptQueue = xQueueCreate(5, sizeof(char *));
    pendingSubscriptions = xQueueCreate(10, sizeof(char *));

    xTaskCreatePinnedToCore(luaTask, "luaTask", 10 * 1024, this, 1, nullptr, 0);
    // ------------------------------------------------------------------------------------

    return payload;
}

// * ============================================================
// * COMMAND HANDLER — executes all MQTT commands (digital, pwm, i2c, spi, uart, ir, batch)
// * ============================================================
void ASHA::handleCommand(JsonVariant doc)
{
    if (doc["delay_ms"].is<int>())
    {
        int delayTime = doc["delay_ms"];
        unsigned long start = millis();
        while (millis() - start < delayTime)
        {
            delay(10);
            if (instance)
                instance->mqttClient.loop();
        }
        Serial.printf("Delayed for %d ms\n", delayTime);
        return;
    }
    int pin = doc["pin"];
    String action = doc["action"];
    int value = doc["value"];

    if (action == "digital")
    {
        if (value == -1)
        {
            int reading = digitalRead(pin);
            Serial.printf("Read Pin %d: Digital %d\n", pin, reading);
            const char *corrID = doc["correlation_id"] | "";
            if (strlen(corrID) > 0)
            {
                JsonDocument response;
                response["correlation_id"] = corrID;
                response["pin"] = pin;
                response["value"] = reading;

                String topic = "asha/response/" + String(instance->currentAshaID.c_str());
                String payload;
                serializeJson(response, payload);
                instance->mqttClient.publish(topic.c_str(), payload.c_str());
            }
        }
        else
        {
            digitalWrite(pin, value);
            Serial.printf("Set Pin %d to Digital %d\n", pin, value);
        }
    }
    else if (action == "pwm")
    {
        int channel = doc["channel"];
        int value = doc["value"] | 0;

        if (value == -1)
        {
            int ledc_duty = ledcRead(channel);
            Serial.printf("Read PWM ch:%d ledc_duty:%d\n", channel, ledc_duty);

            const char *corrID = doc["correlation_id"] | "";
            if (strlen(corrID) > 0)
            {
                JsonDocument response;
                response["correlation_id"] = corrID;
                response["pin"] = pin;
                response["ledc_duty"] = ledc_duty << 3;
                String topic = "asha/response/" + String(instance->currentAshaID.c_str());
                String payload;
                serializeJson(response, payload);
                instance->mqttClient.publish(topic.c_str(), payload.c_str());
            }
        }
        else
        {
            int freq = doc["freq"];
            int duty = doc["duty"];
            ledcSetup(channel, freq, 13);
            ledcAttachPin(pin, channel);
            ledcWrite(channel, duty >> 3);
            Serial.printf("Set Pin %d to PWM ch:%d freq:%d duty:%d\n", pin, channel, freq, duty);
        }
    }

    else if (action == "analog")
    {
        int reading = analogRead(pin);
        Serial.printf("Read Pin %d: Analog %d\n", pin, reading);

        const char *corrID = doc["correlation_id"] | "";
        if (strlen(corrID) > 0)
        {
            JsonDocument response;
            response["correlation_id"] = corrID;
            response["pin"] = pin;
            response["value"] = reading;

            String topic = "asha/response/" + String(instance->currentAshaID.c_str());
            String payload;
            serializeJson(response, payload);
            instance->mqttClient.publish(topic.c_str(), payload.c_str());
        }
    }
    else if (action == "DHT")
    {
        DHT *dht = instance->dhtSensors[pin];
        float temp = dht->readTemperature();
        float humidity = dht->readHumidity();
        Serial.printf("DHT Pin %d: temp=%.1f humidity=%.1f\n", pin, temp, humidity);

        const char *corrID = doc["correlation_id"] | "";
        if (strlen(corrID) > 0)
        {
            JsonDocument response;
            response["correlation_id"] = corrID;
            response["pin"] = pin;
            response["temperature"] = temp;
            response["humidity"] = humidity;

            String topic = "coldGuard/response";
            String payload;
            serializeJson(response, payload);
            instance->mqttClient.publish(topic.c_str(), payload.c_str());
        }
    }
}

// * ============================================================
// * MQTT CALLBACK — routes incoming messages: commands path or topicPayloads store
// * ============================================================
void ASHA::mqttCallback(char *topic, byte *payload, unsigned int length)
{
    instance->lastMqttActivity = millis();
    Serial.print("Message arrived on topic: ");
    Serial.println(topic);

    std::string message;
    for (int i = 0; i < length; i++)
    {
        message += (char)payload[i];
    }
    Serial.print("Payload: ");
    Serial.println(message.c_str());
    std::string topicStr(topic);
    if (topicStr != "coldGuard/command")
    {
        instance->topicPayloads[topicStr] = message;
        return;
    }
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, message);

    if (!error)
    {
        String action = doc["action"];

        if (action == "batch")
        {
            JsonArray commands = doc["commands"];
            for (JsonVariant cmd : commands)
            {
                handleCommand(cmd);
            }
        }
        else if (action == "lua")
        {
            std::string script = doc["script"];
            Serial.println("Received Lua script:");
            Serial.println(script.c_str());
            char *copy = strdup(script.c_str());
            instance->stopLuaRequested = true;
            char *old;
            while (xQueueReceive(instance->luaScriptQueue, &old, 0) == pdTRUE)
            {
                free(old);
            }

            xQueueSend(instance->luaScriptQueue, &copy, 0);
        }
        else
        {
            handleCommand(doc.as<JsonVariant>());
        }
    }
}

// * ============================================================
// * LUA TASK — runs on Core 0, sleeps until a script arrives via queue
// * ============================================================
void ASHA::luaTask(void *param)
{
    ASHA *self = (ASHA *)param;
    while (true)
    {
        char *script;
        if (xQueueReceive(self->luaScriptQueue, &script, portMAX_DELAY) == pdTRUE)
        {
            self->stopLuaRequested = false;
            self->ashaLua.run(script);
            free(script);
        }
    }
}

// * ============================================================
// * MQTT RECONNECT — throttled to one attempt every 5 seconds
// * ============================================================

void ASHA::reconnectMQTT()
{
    unsigned long now = millis();
    if (now - lastReconnectAttempt < 5000)
        return;
    lastReconnectAttempt = now;

    Serial.print("Attempting MQTT connection...");
    String clientId = "ASHA_B_DEVICE-";
    clientId += String(random(0xffff), HEX);
    if (mqttClient.connect(clientId.c_str()))
    {
        Serial.println("connected to MQTT Broker!");
        lastMqttActivity = millis();
        String topic = "coldGuard/command";
        mqttClient.subscribe(topic.c_str());
    }
    else
    {
        Serial.print("failed, rc=");
        Serial.print(mqttClient.state());
        Serial.println(" will retry in 5 seconds");
    }
}
// * =====================================================================
// * SENDING THE PAYLOAD TO THE BACKEND USING HTTP
// * =====================================================================
void ASHA::sendSensorData()
{
    if (isnan(cachedTemp) || isnan(cachedHumidity))
        return;
    HTTPClient http;
    WiFiClient client;
    http.begin(client, SENSOR_API_URL);
    http.addHeader("Content-Type", "application/json");

    JsonDocument doc;
    doc["temperature"] = cachedTemp;
    doc["humidity"] = cachedHumidity;
    doc["light_intensity"] = 0.0;

    String payload;
    serializeJson(doc, payload);

    int code = http.POST(payload);
    Serial.printf(payload.c_str());
    Serial.printf("Sensor POST: %d\n", code);
    http.end();
}

// * ============================================================
// * RUN — called every loop() tick: WiFi watchdog, MQTT reconnect, pending subscriptions
// * ============================================================
void ASHA::run()
{
    if (WiFi.status() != WL_CONNECTED)
    {
        WiFi.reconnect();
        Serial.println("ASHA: WiFi down");
        return;
    }
    if (mqttClient.connected() && (millis() - lastMqttActivity > 1800000))
    {
        Serial.println("ASHA: MQTT watchdog — forcing reconnect");
        mqttClient.disconnect();
        lastReconnectAttempt = 0;
    }
    if (!mqttClient.connected())
    {
        reconnectMQTT();
    }
    if (mqttClient.connected())
    {
        char *subTopic;
        while (xQueueReceive(pendingSubscriptions, &subTopic, 0) == pdTRUE)
        {
            mqttClient.subscribe(subTopic);
            free(subTopic);
        }
    }

    if (millis() - lastDHTRead > 2000)
    {

        lastDHTRead = millis();
        for (auto &entry : dhtSensors)
        {
            float temp = entry.second->readTemperature();
            float hum = entry.second->readHumidity();
            if (!isnan(temp))
                cachedTemp = temp;
            if (!isnan(hum))
                cachedHumidity = hum;
        }
    }

    if (millis() - lastSensorSend > 120000)
    {
        lastSensorSend = millis();
        sendSensorData();
    }

    mqttClient.loop();
}
