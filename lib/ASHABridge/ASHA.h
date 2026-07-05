// you wrote separate classes, in future please put it in one class
#include <ArduinoJson.h>
#include <Lua.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <DHT.h>

#include <map>
#include <string>

enum class DeviceCategory
{
    Actuator,
    Sensor
};

enum class BusType
{
    Digital,
    Analog,
    pwm,
    DHT11,
    DHT22
};

// WIFI
class ASHA_WIFI
{
public:
    void begin(const char *ssid, const char *pwd);
    void onConnect(void (*userdefinedfunc)());

private:
    void (*userdefinedfunLoc)() = nullptr;
};

// the structure that holds the information for a device type
struct DeviceType
{
    DeviceCategory category; // Actuator or Sensor
    std::string metadata;    // "My porch fan"
    BusType bus;             // digital or analog or I2C or PWM
};

// the structure-type that holds all registereddevices like sensor etc
struct RegisteredDevice
{
    int id;
    DeviceType deviceType;
    int pin;
};

// ADDING_DEVICE_CONNECTION
class ASHA_Devices
{
public:
    int addDevice(const DeviceType &deviceType, int pin);
    int getCount() const
    {
        return count;
    }
    RegisteredDevice getDevice(int index) const
    {
        return devices[index];
    }

private:
    static const int MAX_DEVICES = 30;
    RegisteredDevice devices[MAX_DEVICES];
    int count = 0;
};

class ASHA
{
public:
    ASHA_WIFI asha_wifi;
    ASHA_Devices asha_devices;
    std::string init(const std::string &ashaID);
    DeviceType genericDev(DeviceCategory deviceCategory, const std::string &metadata,
                          BusType busType);
    void run();

    static void handleCommand(JsonVariant doc);
    static ASHA *getInstance()
    {
        return instance;
    }
    void subscribeTopic(const char *topic);
    const std::string gettopicPayload(const char *topic)
    {
        return topicPayloads[topic];
    }
    bool getStopLuaRequested()
    {
        return stopLuaRequested;
    }
    std::map<int, DHT *> dhtSensors;
    unsigned long lastDHTRead = 0;
    float cachedTemp = NAN;
    float cachedHumidity = NAN;
    unsigned long lastSensorSend = 0;
    void sendSensorData();

private:
    WiFiClient espClient;
    PubSubClient mqttClient;
    std::string currentAshaID;
    unsigned long lastReconnectAttempt = 0;
    static ASHA *instance;
    Lua ashaLua;
    QueueHandle_t luaScriptQueue;
    static void luaTask(void *param);
    unsigned long lastMqttActivity = 0;
    std::map<std::string, std::string> topicPayloads;
    QueueHandle_t pendingSubscriptions;
    volatile bool stopLuaRequested = false;

    void reconnectMQTT();
    static void mqttCallback(char *topic, byte *payload, unsigned int length);
};
