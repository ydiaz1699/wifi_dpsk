/**
 * Central IoT — wifi_dpsk
 *
 * Loop (prioridades):
 *   1. WiFi
 *   2. IoTNode.loop() → UDP, ACK, dedup, despacho
 *   3. loopHttpEndpoint() → satélites ESPHome por HTTP
 *   4. Buzzer timer
 *   5. MQTT (modo LOCAL/HA) + drenaje de mqttQueue
 */

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ArduinoOTA.h>
#include <IoTNode.h>
#include <IoTStorage.h>
#include <IoTAuth.h>
#include "config.h"
#include "hal.h"
#include "logger.h"
#include "mqtt_manager.h"
#include "mqtt_publish_queue.h"
#include "http_endpoint.h"
#include "event_handler.h"
#include <cstring>

static const char FW_VERSION[] = "4.3.0";
static const unsigned long HEALTH_LOG_INTERVAL_MS = 30000;
static const unsigned long STORAGE_RETRY_INTERVAL_MS = 300000;
static unsigned long lastHealthLog = 0;
static unsigned long nextStorageRetry = 0;
static uint32_t minimumFreeHeap = 0xFFFFFFFFUL;

static const char* bootReasonToString(BootReason reason) {
    switch (reason) {
        case BootReason::POWER_ON:       return "POWER_ON";
        case BootReason::SOFTWARE_RESET: return "SOFTWARE_RESET";
        case BootReason::WATCHDOG:       return "WATCHDOG";
        case BootReason::DEEP_SLEEP:     return "DEEP_SLEEP";
        case BootReason::OTA_UPDATE:     return "OTA_UPDATE";
        case BootReason::CRASH:          return "CRASH";
        default:                         return "UNKNOWN";
    }
}

static BootReason detectBootReason() {
    const String resetInfo = ESP.getResetReason();
    const char* text = resetInfo.c_str();
    if (std::strstr(text, "wdt") || std::strstr(text, "WDT") ||
        std::strstr(text, "Watchdog")) return BootReason::WATCHDOG;
    if (std::strstr(text, "Exception") || std::strstr(text, "Fatal")) {
        return BootReason::CRASH;
    }
    if (std::strstr(text, "Deep-Sleep") || std::strstr(text, "Deep Sleep")) {
        return BootReason::DEEP_SLEEP;
    }
    if (std::strstr(text, "Power")) return BootReason::POWER_ON;
    if (std::strstr(text, "Software") || std::strstr(text, "restart")) {
        return BootReason::SOFTWARE_RESET;
    }
    return BootReason::UNKNOWN;
}

static void monitorRuntimeHealth() {
    const uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < minimumFreeHeap) minimumFreeHeap = freeHeap;
    const unsigned long now = millis();
    if (now - lastHealthLog >= HEALTH_LOG_INTERVAL_MS) {
        lastHealthLog = now;
        LOG_INFO("Health: heap=%lu min=%lu storage=%s mqttQ=%u/%u mqttDrops=%lu",
                 (unsigned long)freeHeap,
                 (unsigned long)minimumFreeHeap,
                 storage.isMounted() ? "mounted" : "degraded",
                 mqttQueue.count(), MqttPublishQueue::CAPACITY,
                 (unsigned long)mqttQueue.drops());
    }
}

static void retryStorageIfNeeded() {
    if (storage.isMounted()) return;
    const unsigned long now = millis();
    if (static_cast<long>(now - nextStorageRetry) < 0) return;
    nextStorageRetry = now + STORAGE_RETRY_INTERVAL_MS;
    if (storage.retryMount()) {
        LOG_WARN("Storage recuperado; BOOT_ID de este arranque sigue degradado");
    } else {
        LOG_WARN("Storage sigue degradado; proximo reintento en %lums",
                 STORAGE_RETRY_INTERVAL_MS);
    }
}

// --- Shared secret (desde secrets.h, NO versionado) ---
static const uint8_t AUTH_KEY[] = IOT_AUTH_KEY;

// --- Hardware ---
Led led(pinLed);
Buzzer buzzer(pinBocina);

// --- IoTProtocol ---
IoTStorage storage;
IoTNode node(MY_DEVICE_ID, IOT_UDP_PORT);
IoTAuth auth(AUTH_KEY, IOT_AUTH_KEY_LEN);

static bool verifyAuthPacket(const IoTPacket &pkt, void *context) {
    return static_cast<IoTAuth*>(context)->verifyPacket(pkt);
}

static bool signAuthPacket(IoTPacket &pkt, void *context) {
    return static_cast<IoTAuth*>(context)->signPacket(pkt);
}

static void configureAuthProvider(bool enabled) {
    IoTAuthProvider provider{};
    provider.mode = enabled ? IoTAuthMode::REQUIRED : IoTAuthMode::DISABLED;
    provider.signOutgoing = enabled;
    provider.verify = verifyAuthPacket;
    provider.sign = signAuthPacket;
    provider.onRejected = nullptr;
    provider.context = &auth;
    node.setAuthProvider(provider);
}

// --- Estado ---
String modoAlarma = "armado";

// --- WiFi ---
static bool wifiConectando = false;
static unsigned long ultimoIntentoWiFi = 0;

static bool stateRequestPending = true;
static uint8_t stateRequestAttempts = 0;
static unsigned long nextStateRequestAt = 0;
static bool stateRequestWifiWasConnected = false;
static const uint8_t STATE_REQUEST_MAX_ATTEMPTS = 3;

static unsigned long stateRequestDelay(uint8_t attempt) {
    switch (attempt) {
        case 0: return 0;
        case 1: return 3000;
        default: return 7000;
    }
}

static void iniciarWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.config(local_IP, gateway_IP, subnet_mask);
    WiFi.begin(ssid, password);
    wifiConectando = true;
    ultimoIntentoWiFi = millis();
    LOG_INFO("WiFi conectando...");
}

static void manejarWiFi() {
    if (WiFi.status() == WL_CONNECTED) {
        if (wifiConectando) {
            wifiConectando = false;
            LOG_INFO("WiFi OK: %s", WiFi.localIP().toString().c_str());
        }
        return;
    }
    unsigned long ahora = millis();
    if (!wifiConectando) {
        iniciarWiFi();
    } else if (ahora - ultimoIntentoWiFi > 5000) {
        LOG_WARN("WiFi reintentando...");
        iniciarWiFi();
    }
}

// --- OTA ---
static void setupOTA() {
    ArduinoOTA.setHostname(CENTRAL_OTA_HOSTNAME);
    ArduinoOTA.setPort(CENTRAL_OTA_PORT);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]() { LOG_INFO("OTA Start"); });
    ArduinoOTA.onEnd([]() { LOG_INFO("OTA End"); });
    ArduinoOTA.onError([](ota_error_t err) { LOG_ERROR("OTA Error %u", err); });
    ArduinoOTA.begin();
    LOG_INFO("OTA listo: %s:%u", CENTRAL_OTA_HOSTNAME, CENTRAL_OTA_PORT);
}

// ============================================================
// SETUP
// ============================================================

void setup() {
    Serial.begin(115200);
    delay(100);
    LOG_INFO("===== Central IoT wifi_dpsk V4.3 =====");
    const BootReason bootReason = detectBootReason();
    LOG_INFO("Reset reason: %s (%s), FW=%s",
             bootReasonToString(bootReason), ESP.getResetReason().c_str(),
             FW_VERSION);

    ESP.wdtEnable(8000);

    led.begin();
    buzzer.begin();
    buzzer.setLed(&led);

    // --- Storage ---
    const bool storageReady = storage.begin();
    if (storageReady) {
        if (!storage.loadConfig()) {
            LOG_WARN("Config ausente o invalida: usando defaults");
        }
    } else {
        nextStorageRetry = millis() + STORAGE_RETRY_INTERVAL_MS;
        LOG_ERROR("Storage FAIL: usando defaults; montaje no destructivo");
    }

    const uint16_t bootId = storage.getBootId();
    if (storageReady) {
        LOG_INFO("Storage OK: boot#%lu bootId=0x%04X",
                 (unsigned long)storage.getBootCount(), bootId);
    } else {
        LOG_WARN("BOOT_ID no persistente por fallo de Storage: 0x%04X", bootId);
    }
    if (!storage.isBootIdPersistent()) {
        LOG_ERROR("BOOT_ID degradado: la sesion puede repetirse tras otro reinicio");
    }

    if (storage.config().authEnabled) {
        auth.setRequired(true);
        LOG_INFO("Auth HMAC: REQUERIDO");
    } else {
        auth.setRequired(false);
        LOG_INFO("Auth HMAC: deshabilitado (bypass completo)");
    }

    iniciarWiFi();

    LOG_INFO("Esperando WiFi...");
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
        ESP.wdtFeed();
        delay(100);
    }

    node.begin(bootId);
    configureAuthProvider(storage.config().authEnabled);
    node.onPacketReceived(handleIoTPacket);
    LOG_INFO("IoTNode (ID=0x%02X, bootId=0x%04X, puerto=%d)",
             MY_DEVICE_ID, node.getBootId(), IOT_UDP_PORT);

    inicializarMQTT();
    setupOTA();

    // Endpoint HTTP de emergencia para satélites ESPHome.
    // Se habilita solo si auth está desactivado (ver http_endpoint.cpp).
    setupHttpEndpoint();

    stateRequestPending = true;
    stateRequestAttempts = 0;
    nextStateRequestAt = millis();
    stateRequestWifiWasConnected = false;

    LOG_INFO("Modo MQTT: %s | Alarma: %s", modoMQTTStr(), modoAlarma.c_str());
    LOG_INFO("Setup completo — esperando eventos...");
}

// --- STATE_SYNC ---
static void enviarStateRequestSiCorresponde() {
    if (!stateRequestPending || WiFi.status() != WL_CONNECTED) return;

    const unsigned long ahora = millis();
    if (static_cast<long>(ahora - nextStateRequestAt) < 0) return;

    IoTPacket req{};
    req.version = IOT_PROTOCOL_VER;
    req.type = MsgType::STATE_REQUEST;
    req.src = MY_DEVICE_ID;
    req.dst = IOT_DEVICE_BROADCAST;
    req.bootId = node.getBootId();
    req.seq = node.getNextSeq();
    req.flags = 0;
    req.clearPayload();

    const IPAddress broadcast = WiFi.broadcastIP();
    node.sendDirect(req, broadcast, IOT_UDP_PORT);
    LOG_INFO("STATE_REQUEST broadcast emitido (%s), intento %u/%u",
             broadcast.toString().c_str(),
             static_cast<unsigned>(stateRequestAttempts + 1),
             static_cast<unsigned>(STATE_REQUEST_MAX_ATTEMPTS));

    stateRequestAttempts++;
    if (stateRequestAttempts >= STATE_REQUEST_MAX_ATTEMPTS) {
        stateRequestPending = false;
        return;
    }
    nextStateRequestAt = ahora + stateRequestDelay(stateRequestAttempts);
}

// ============================================================
// LOOP
// ============================================================

void loop() {
    ESP.wdtFeed();
    monitorRuntimeHealth();
    retryStorageIfNeeded();

    manejarWiFi();

    const bool wifiConectado = WiFi.status() == WL_CONNECTED;
    if (!wifiConectado) {
        mqttDisponible = false;
        stateRequestWifiWasConnected = false;
    } else if (!stateRequestWifiWasConnected) {
        stateRequestWifiWasConnected = true;
        stateRequestPending = true;
        stateRequestAttempts = 0;
        nextStateRequestAt = millis();
    }

    // PRIORIDAD #1: IoTProtocol UDP (nunca bloquea)
    node.loop();

    // Endpoint HTTP de satélites ESPHome, después de UDP y antes del buzzer.
    loopHttpEndpoint();

    // Timer del buzzer después de recibir/despachar eventos.
    buzzer.loop();

    enviarStateRequestSiCorresponde();

    // PRIORIDAD #2: MQTT (también se llama sin WiFi para limpiar disponibilidad)
    manejarMQTT();

    if (consumirSolicitudStateSync()) {
        stateRequestPending = true;
        stateRequestAttempts = 0;
        nextStateRequestAt = millis();
    }

    static bool lastBuzzer = false;
    if (buzzer.isOn() != lastBuzzer) {
        lastBuzzer = buzzer.isOn();
        publicarEstadoBocina();
    }

    static unsigned long lastStatusPub = 0;
    if (millis() - lastStatusPub >= 30000) {
        lastStatusPub = millis();
        if (mqttDisponible) {
            for (uint8_t id = 0x02; id <= 0x7F; id++) {
                RemoteDevice* dev = node.getRemote(id);
                if (!dev) continue;
                char topic[64];
                snprintf(topic, sizeof(topic), "casa/iot/device_%02X/status", dev->id);
                const char* stateStr = "unknown";
                switch (dev->state) {
                    case DeviceState::ONLINE:  stateStr = "online"; break;
                    case DeviceState::STALE:   stateStr = "stale"; break;
                    case DeviceState::OFFLINE: stateStr = "offline"; break;
                    default: break;
                }
                mqttQueue.push(topic, stateStr, true,
                               MqttPublishQueue::PRIO_BACKGROUND);
            }
        }
    }

    ArduinoOTA.handle();
}
