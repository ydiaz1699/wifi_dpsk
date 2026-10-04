#include "mqtt_publish_queue.h"
#include <string.h>

MqttPublishQueue mqttQueue;

void MqttPublishQueue::_copyString(char* dst, size_t cap, const char* src) {
    if (cap == 0) return;
    strncpy(dst, src ? src : "", cap - 1);
    dst[cap - 1] = '\0';
}

int MqttPublishQueue::_findFreeSlot() const {
    for (uint8_t i = 0; i < CAPACITY; i++) {
        if (!_items[i].used) return i;
    }
    return -1;
}

int MqttPublishQueue::_findCoalesceTarget(const char* topic) const {
    for (uint8_t i = 0; i < CAPACITY; i++) {
        if (_items[i].used && strcmp(_items[i].topic, topic) == 0) {
            return i;
        }
    }
    return -1;
}

int MqttPublishQueue::_findVictimForOverflow(uint8_t newPriority) const {
    if (newPriority >= PRIO_BACKGROUND) return -1;
    int victim = -1;
    uint8_t worstPrio = newPriority;
    for (uint8_t i = 0; i < CAPACITY; i++) {
        if (!_items[i].used) continue;
        if (_items[i].priority > worstPrio) {
            victim = i;
            worstPrio = _items[i].priority;
        }
    }
    return victim;
}

bool MqttPublishQueue::push(const char* topic, const char* payload,
                             bool retained, uint8_t priority) {
    if (!topic || topic[0] == '\0') { _drops++; return false; }

    // Coalescing: si ya hay un item con el mismo topic, reemplaza payload.
    int coalesce = _findCoalesceTarget(topic);
    if (coalesce >= 0) {
        _copyString(_items[coalesce].payload, sizeof(_items[coalesce].payload), payload);
        _items[coalesce].retained = retained;
        if (priority < _items[coalesce].priority) {
            _items[coalesce].priority = priority;
        }
        _items[coalesce].enqueued_ms = millis();
        return true;
    }

    int slot = _findFreeSlot();
    if (slot < 0) {
        slot = _findVictimForOverflow(priority);
        if (slot < 0) { _drops++; return false; }
    } else {
        _count++;
    }

    PendingPublish& item = _items[slot];
    _copyString(item.topic, sizeof(item.topic), topic);
    _copyString(item.payload, sizeof(item.payload), payload);
    item.retained = retained;
    item.priority = priority;
    item.enqueued_ms = millis();
    item.used = true;
    return true;
}

bool MqttPublishQueue::pop(PendingPublish& out) {
    if (_count == 0) return false;

    int best = -1;
    uint8_t bestPrio = 0xFF;
    uint32_t bestAge = 0xFFFFFFFF;
    for (uint8_t i = 0; i < CAPACITY; i++) {
        if (!_items[i].used) continue;
        const bool higherPriority = _items[i].priority < bestPrio;
        const bool samePriorityOlder = _items[i].priority == bestPrio &&
                                        _items[i].enqueued_ms < bestAge;
        if (best < 0 || higherPriority || samePriorityOlder) {
            best = i;
            bestPrio = _items[i].priority;
            bestAge = _items[i].enqueued_ms;
        }
    }
    if (best < 0) return false;

    out = _items[best];
    _items[best].used = false;
    _count--;
    return true;
}

void MqttPublishQueue::requeue(const PendingPublish& item) {
    int slot = _findFreeSlot();
    if (slot < 0) {
        slot = _findVictimForOverflow(item.priority);
        if (slot < 0) { _drops++; return; }
    } else {
        _count++;
    }
    _items[slot] = item;
    _items[slot].used = true;
    _items[slot].enqueued_ms = millis();
}

uint8_t MqttPublishQueue::depthCritical() const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < CAPACITY; i++) {
        if (_items[i].used && _items[i].priority == PRIO_CRITICAL) n++;
    }
    return n;
}

uint8_t MqttPublishQueue::depthNormal() const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < CAPACITY; i++) {
        if (_items[i].used && _items[i].priority == PRIO_NORMAL) n++;
    }
    return n;
}

uint8_t MqttPublishQueue::depthBackground() const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < CAPACITY; i++) {
        if (_items[i].used && _items[i].priority == PRIO_BACKGROUND) n++;
    }
    return n;
}
