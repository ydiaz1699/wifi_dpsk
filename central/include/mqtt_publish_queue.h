#pragma once
#include <Arduino.h>
#include <stdint.h>

// Cola de publicación MQTT diferida, con prioridades y coalescing.
//
// Objetivo: desacoplar la recepción UDP (que corre en node.loop() con
// presupuesto y debe responder ACK cuanto antes) de la publicación MQTT
// (que puede bloquear varios ms si el broker está lento o la cola TCP
// tiene backpressure).
//
// Política:
//   - Prioridad 0 (crítico): eventos de alarma. Nunca se descarta en
//     favor de otro; si la cola está llena, se descarta el de menor
//     prioridad que exista.
//   - Prioridad 1 (normal): estados retained (bocina, modo).
//   - Prioridad 2 (background): telemetría y heartbeats. Descartables.
//   - Coalescing por topic: si ya hay un item con el mismo topic, se
//     reemplaza el payload en lugar de encolar un duplicado.

struct PendingPublish {
    char     topic[80];
    char     payload[80];
    bool     retained;
    uint8_t  priority;    // 0=crítico, 1=normal, 2=background
    bool     used;
    uint32_t enqueued_ms;
};

class MqttPublishQueue {
public:
    static constexpr uint8_t CAPACITY = 24;

    static constexpr uint8_t PRIO_CRITICAL   = 0;
    static constexpr uint8_t PRIO_NORMAL     = 1;
    static constexpr uint8_t PRIO_BACKGROUND = 2;

    // Encola (o reemplaza por coalescing). Devuelve true si quedó en cola.
    bool push(const char* topic, const char* payload, bool retained,
              uint8_t priority);

    // Extrae el de mayor prioridad (menor número). Si dos tienen la misma
    // prioridad, sale el más antiguo (FIFO dentro de la misma prioridad).
    bool pop(PendingPublish& out);

    // Reencola un item que no se pudo publicar (típicamente retained).
    void requeue(const PendingPublish& item);

    uint8_t  count() const { return _count; }
    uint32_t drops() const { return _drops; }
    uint32_t publishes() const { return _publishes; }
    uint8_t  depthCritical() const;
    uint8_t  depthNormal() const;
    uint8_t  depthBackground() const;

    void markPublished() { _publishes++; }

private:
    PendingPublish _items[CAPACITY] = {};
    uint8_t  _count = 0;
    uint32_t _drops = 0;
    uint32_t _publishes = 0;

    int _findFreeSlot() const;
    int _findCoalesceTarget(const char* topic) const;
    int _findVictimForOverflow(uint8_t newPriority) const;
    static void _copyString(char* dst, size_t cap, const char* src);
};

// Instancia global única usada por event_handler.cpp y mqtt_manager.cpp.
extern MqttPublishQueue mqttQueue;
