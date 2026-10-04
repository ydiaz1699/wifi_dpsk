#pragma once
#include <ESP8266WiFi.h>

inline IPAddress redGateway() { return IPAddress(192, 168, 0, 1); }
inline IPAddress redSubnet()  { return IPAddress(255, 255, 255, 0); }

const unsigned int PUERTO_UDP = 4210;
