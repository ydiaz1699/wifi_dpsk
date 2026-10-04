#include <ArduinoOTA.h>
#include <ESP8266WiFi.h>

#include "ota.h"
#include "device_config.h"
#include "logger.h"
#include "secrets.h"

namespace {

bool otaInicializada = false;
volatile bool actualizacionEnProgreso = false;
unsigned int ultimoPorcentaje = 0;

}  // namespace

void setupOTA() {
    if (otaInicializada || WiFi.status() != WL_CONNECTED) {
        return;
    }

    ArduinoOTA.setHostname(EMISOR_OTA_HOSTNAME);
    ArduinoOTA.setPort(EMISOR_OTA_PORT);
    ArduinoOTA.setPassword(OTA_PASSWORD);

    ArduinoOTA.onStart([]() {
        actualizacionEnProgreso = true;
        ultimoPorcentaje = 0;
        const char* tipo = ArduinoOTA.getCommand() == U_FLASH ? "firmware" : "filesystem";
        LOG_WARN("OTA iniciada: %s; sensores pausados durante la actualización", tipo);
    });

    ArduinoOTA.onEnd([]() {
        LOG_INFO("OTA finalizada; el equipo se reiniciará");
    });

    ArduinoOTA.onProgress([](unsigned int progreso, unsigned int total) {
        if (total == 0) {
            return;
        }

        unsigned int porcentaje = (static_cast<uint32_t>(progreso) * 100U) / total;
        if (porcentaje == 100U || porcentaje >= ultimoPorcentaje + 10U) {
            ultimoPorcentaje = porcentaje;
            LOG_INFO("OTA progreso: %u%%", porcentaje);
        }
    });

    ArduinoOTA.onError([](ota_error_t error) {
        actualizacionEnProgreso = false;
        LOG_ERROR("OTA error: %u; se reanuda la aplicación", error);
    });

    ArduinoOTA.begin();
    otaInicializada = true;
    LOG_INFO("OTA lista: host=%s puerto=%u", EMISOR_OTA_HOSTNAME, EMISOR_OTA_PORT);
}

void handleOTA() {
    if (otaInicializada) {
        ArduinoOTA.handle();
    }
}

bool otaEnProgreso() {
    return actualizacionEnProgreso;
}
