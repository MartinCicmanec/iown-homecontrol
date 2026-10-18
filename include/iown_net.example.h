// Copy to include/iown_net.h (git-ignored) and fill in your own values.
// include/iown_net.h holds WiFi and MQTT credentials and must never be committed.
#pragma once

#define IOWN_WIFI_SSID   "your-ssid"
#define IOWN_WIFI_PASS   "your-wifi-password"

#define IOWN_MQTT_HOST   "192.168.1.10"   // Home Assistant / Mosquitto broker IP
#define IOWN_MQTT_PORT   1883
#define IOWN_MQTT_USER   "mqtt-user"       // broker username ("" if anonymous)
#define IOWN_MQTT_PASS   "mqtt-password"   // broker password ("" if anonymous)
#define IOWN_MQTT_CLIENT "iown-lora32"
