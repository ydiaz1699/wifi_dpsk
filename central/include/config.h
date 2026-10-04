#pragma once
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include "secrets.h"

// --- Red ---
extern const char* ssid;
extern const char* password;
extern IPAddress local_IP;
extern IPAddress gateway_IP;
extern IPAddress subnet_mask;

// --- MQTT ---
extern const char* mqtt_server;
extern const int mqtt_port;
extern const char* mqtt_client_id;
extern const char* mqtt_user;
extern const char* mqtt_pass;

// --- MQTT Topics ---
extern const char* TOPIC_ESTADO;
extern const char* TOPIC_UPTIME;
extern const char* TOPIC_IP;
extern const char* TOPIC_MODO;
extern const char* TOPIC_MODO_STATE;
extern const char* TOPIC_BOCINA_CMD;
extern const char* TOPIC_BOCINA_STATE;

// Compatibilidad MQTT/HA con el contrato probado de V3.
extern const char* TOPIC_V3_EVENTO;
extern const char* TOPIC_V3_TIMBRE;
extern const char* TOPIC_V3_ESTADO;
extern const char* TOPIC_V3_BOCINA_CMD;
extern const char* TOPIC_V3_BOCINA_STATE;
extern const char* TOPIC_V3_MODO;
extern const char* TOPIC_V3_MODO_STATE;
extern const char* TOPIC_V3_UPTIME;
extern const char* TOPIC_V3_IP;

// --- Hardware ---
extern const int pinBocina;
extern const int pinLed;

// --- IoTProtocol ---
extern const uint16_t IOT_UDP_PORT;
extern const uint8_t MY_DEVICE_ID;

// --- Timings ---
extern const unsigned long DURACION_BOCINA_MOTION_MS;
extern const unsigned long DURACION_BOCINA_TIMBRE_MS;
extern const unsigned long DURACION_BOCINA_PUERTA_MS;
extern const unsigned long MQTT_RECONNECT_INTERVAL_MS;
extern const unsigned long MQTT_SONDEO_INTERVAL_MS;
extern const unsigned long MQTT_SONDEO_DESPUES_DE_CAIDA_MS;

// --- OTA ---
// Añadido en wifi_dpsk: el main.cpp de la central usa estos símbolos para
// configurar ArduinoOTA (hostname + puerto). El receptor original llamaba a
// setHostname("central-iot") sin puerto; aquí se centraliza en config.h.
#define CENTRAL_OTA_HOSTNAME "wifi-dpsk-central"
#define CENTRAL_OTA_PORT     8266
