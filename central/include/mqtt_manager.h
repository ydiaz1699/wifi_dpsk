#pragma once
#include <PubSubClient.h>

extern PubSubClient mqtt;
extern bool mqttDisponible;

// Modos de conexión MQTT (igual que V3.3)
enum class ModoMQTT { MODO_LOCAL, MODO_HA };
extern ModoMQTT modoMQTT;

void inicializarMQTT();
void manejarMQTT();
// True una sola vez después de cada conexión MQTT exitosa, para que la
// central vuelva a solicitar estados que pudieron llegar durante una caída.
bool consumirSolicitudStateSync();
void publicarEstadoBocina();
const char* modoMQTTStr();
