#pragma once
#include <ESP8266WiFi.h>
#include <IoTProtocol.h>

/**
 * Handler principal de paquetes IoTProtocol recibidos.
 * Procesamiento genérico: no hardcodea sensores.
 * Llamado por IoTNode cuando un paquete válido (no duplicado) llega.
 */
void handleIoTPacket(const IoTPacket &pkt, IPAddress remoteIP, uint16_t remotePort);
