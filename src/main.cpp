#include <Arduino.h>
#include <ASHA.h>

ASHA asha;

void afterConnection()
{
    Serial.println("ASHA: Internet Connected ");
}

// for when internet is connected to stream data to cloud, otherwise you can just skip but I'd advice you set it
const char *ssid = "your wifi name";
const char *password = "your wifi password";

void setup()
{
    asha.asha_wifi.onConnect(afterConnection);
    asha.asha_wifi.begin(ssid, password);
    asha.asha_devices.addDevice(
        asha.genericDev(DeviceCategory::Actuator, "Red Light", BusType::Digital), 18);
    asha.asha_devices.addDevice(
        asha.genericDev(DeviceCategory::Actuator, "Green Light", BusType::Digital), 19);
    asha.asha_devices.addDevice(
        asha.genericDev(DeviceCategory::Actuator, "Buzzer", BusType::pwm), 21);
    asha.asha_devices.addDevice(
        asha.genericDev(DeviceCategory::Sensor, "My DHT11 sensor", BusType::DHT11), 22);
    asha.asha_devices.addDevice(
        asha.genericDev(DeviceCategory::Actuator, "My fan", BusType::Digital), 23);
    asha.init("dc9e148e-e97d-4d55-839a-4ff927a87b41");
};

void loop()
{
    asha.run();
}
