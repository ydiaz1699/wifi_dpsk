#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <string>
#include "config.h"
#include "logger.h"
#include "mqtt_discovery.h"

extern PubSubClient mqtt;

namespace {

constexpr const char* DEVICE_ID   = "central_alarma_iot";
constexpr const char* DEVICE_NAME = "Central Alarma IoT";
constexpr const char* AVAIL_TOPIC = "casa/alarma/estado";  // el LWT

// Bloque `device` compartido por las 7 entidades.
void agregarDevice(JsonObject& o) {
    JsonObject dev = o.createNestedObject("device");
    JsonArray ids = dev.createNestedArray("identifiers");
    ids.add(DEVICE_ID);
    dev["name"] = DEVICE_NAME;
    dev["manufacturer"] = "Casero";
    dev["model"] = "ESP8266 NodeMCU";
    dev["sw_version"] = "4.3.0";
}

struct EntitySpec {
    const char* component;
    const char* unique_suffix;
    const char* name;
    const char* state_topic;      // absoluto (contrato V3 casa/alarma/*)
    const char* command_topic;    // nullptr si es read-only
    const char* payload_on;
    const char* payload_off;
    const char* device_class;
    const char* entity_category;
};

void publicarEntidad(const EntitySpec& spec) {
    StaticJsonDocument<768> doc;
    JsonObject o = doc.to<JsonObject>();

    o["unique_id"] = std::string(DEVICE_ID) + "_" + spec.unique_suffix;
    o["name"] = spec.name;
    o["state_topic"] = spec.state_topic;
    if (spec.command_topic) o["command_topic"] = spec.command_topic;
    if (spec.payload_on)    o["payload_on"]  = spec.payload_on;
    if (spec.payload_off)   o["payload_off"] = spec.payload_off;
    if (spec.device_class)  o["device_class"] = spec.device_class;
    if (spec.entity_category) o["entity_category"] = spec.entity_category;

    // Es un select: necesita options.
    if (std::string(spec.component) == "select") {
        JsonArray options = o.createNestedArray("options");
        options.add("armado");
        options.add("desarmado");
    }

    // Availability como lista, para que HA marque la entidad "unavailable"
    // si el LWT está en offline.
    JsonArray avail = o.createNestedArray("availability");
    JsonObject a1 = avail.createNestedObject();
    a1["topic"] = AVAIL_TOPIC;
    a1["payload_available"] = "online";
    a1["payload_not_available"] = "offline";

    agregarDevice(o);

    char topic[128];
    snprintf(topic, sizeof(topic), "homeassistant/%s/%s_%s/config",
             spec.component, DEVICE_ID, spec.unique_suffix);

    char payload[768];
    size_t n = serializeJson(doc, payload, sizeof(payload));
    if (n == 0) {
        LOG_WARN("Discovery serialize fallo: %s", topic);
        return;
    }
    if (!mqtt.publish(topic, (const uint8_t*)payload, n, /*retained=*/true)) {
        LOG_WARN("Discovery publish fallo: %s", topic);
    }
}

}  // namespace

void publicarDiscovery() {
    // Las 7 entidades heredan los topics V3 desde casa/alarma/*, que es
    // el contrato que HA ya consumía en V3. Los topics V4 casa/iot/central/*
    // siguen publicándose en paralelo desde mqtt_manager.cpp.
    const EntitySpec entidades[] = {
        { "binary_sensor", "pir_evento",     "Alarma - Movimiento PIR",
          "casa/alarma/evento", nullptr,
          "detectado", nullptr, "motion", nullptr },
        { "binary_sensor", "timbre_evento",  "Alarma - Timbre",
          "casa/alarma/timbre", nullptr,
          "presionado", nullptr, "occupancy", nullptr },
        { "binary_sensor", "online",         "Central Alarma - Online",
          "casa/alarma/estado", nullptr,
          "online", "offline", "connectivity", nullptr },
        { "switch",        "bocina_manual",  "Alarma - Forzar Bocina",
          "casa/alarma/bocina/state", "casa/alarma/bocina/set",
          "ON", "OFF", nullptr, nullptr },
        { "select",        "modo",           "Alarma - Modo",
          "casa/alarma/modo/state", "casa/alarma/modo/set",
          nullptr, nullptr, nullptr, nullptr },
        { "sensor",        "uptime",         "Central Alarma - Uptime",
          "casa/alarma/uptime", nullptr,
          nullptr, nullptr, nullptr, "diagnostic" },
        { "sensor",        "ip",             "Central Alarma - IP",
          "casa/alarma/ip", nullptr,
          nullptr, nullptr, nullptr, "diagnostic" },
    };

    for (const auto& spec : entidades) {
        publicarEntidad(spec);
    }

    LOG_INFO("Discovery MQTT publicado: 7 entidades retained");
}
