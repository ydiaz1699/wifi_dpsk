#include "http_endpoint.h"
#include <ESP8266WebServer.h>
#include <ArduinoJson.h>
#include <IoTNode.h>
#include <IoTStorage.h>
#include "config.h"
#include "logger.h"
#include "event_handler.h"

extern IoTNode node;
extern IoTStorage storage;

static ESP8266WebServer httpServer(80);

// El endpoint solo se habilita cuando la política de auth es DISABLED.
// En modo REQUIRED, el canal HTTP no ofrece HMAC por paquete y se cierra.
static bool httpEndpointPermitido() {
    return !storage.config().authEnabled;
}

static void sendJson(int code, const char* json) {
    httpServer.send(code, "application/json", json);
}

static void handleEmergencyEvent() {
    if (!httpEndpointPermitido()) {
        LOG_WARN("HTTP /event rechazado: auth REQUIRED activo");
        sendJson(403, "{\"ok\":false,\"reason\":\"auth_required\"}");
        return;
    }

    if (!httpServer.hasArg("plain")) {
        sendJson(400, "{\"ok\":false,\"reason\":\"no_body\"}");
        return;
    }

    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, httpServer.arg("plain"));
    if (err) {
        LOG_WARN("HTTP /event JSON invalido: %s", err.c_str());
        sendJson(400, "{\"ok\":false,\"reason\":\"bad_json\"}");
        return;
    }

    uint8_t  src   = doc["src"]  | 0;
    uint16_t boot  = doc["boot"] | 0;
    uint32_t seq   = doc["seq"]  | 0;
    uint8_t  code  = doc["code"] | 0;
    uint8_t  value = doc["val"]  | 0;

    if (src == 0 || boot == 0 || seq == 0 || code == 0) {
        sendJson(400, "{\"ok\":false,\"reason\":\"missing_fields\"}");
        return;
    }

    IPAddress remoteIP = httpServer.client().remoteIP();
    // InjectResult es un enum de ÁMBITO GLOBAL (declarado fuera de IoTNode en
    // IoTNode.h), igual que DeviceState o QueueOverflow. NO es IoTNode::InjectResult.
    InjectResult r = node.injectEvent(src, boot, seq, code, value, remoteIP);

    switch (r) {
        case InjectResult::ACCEPTED:
            LOG_INFO("HTTP /event aceptado: src=0x%02X boot=0x%04X seq=%lu code=%u",
                     src, boot, (unsigned long)seq, code);
            sendJson(200, "{\"ok\":true}");
            break;
        case InjectResult::DUPLICATE:
            LOG_DEBUG("HTTP /event duplicado: src=0x%02X seq=%lu",
                      src, (unsigned long)seq);
            sendJson(409, "{\"ok\":false,\"reason\":\"duplicate\"}");
            break;
        case InjectResult::REJECTED:
        default:
            LOG_WARN("HTTP /event rechazado: src=0x%02X boot=0x%04X seq=%lu",
                     src, boot, (unsigned long)seq);
            sendJson(403, "{\"ok\":false,\"reason\":\"rejected\"}");
            break;
    }
}

void setupHttpEndpoint() {
    httpServer.on("/event", HTTP_POST, handleEmergencyEvent);
    httpServer.onNotFound([]() {
        sendJson(404, "{\"ok\":false,\"reason\":\"not_found\"}");
    });
    httpServer.begin();
    LOG_INFO("HTTP /event en puerto 80 (auth=%s)",
             httpEndpointPermitido() ? "DISABLED" : "REQUIRED -> cerrado");
}

void loopHttpEndpoint() {
    httpServer.handleClient();
}
