#pragma once

// Publica las entidades Home Assistant del contrato MQTT compatible con V3.
// Se mantiene fuera de IoTProtocol: solo conoce MQTT y el modelo de la central.
void publicarDiscovery();
