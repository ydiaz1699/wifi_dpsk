/**
 * MQTT Manager V4.3 (wifi_dpsk) — Modo dual LOCAL/HA + cola diferida
 *
 * - El primer intento se difiere al loop, después de atender IoTNode.
 * - Si el broker responde → MODO_HA; si no → MODO_LOCAL.
 * - En LOCAL sondea cada 5 min; en HA reintenta cada 15 s.
 * - Nunca intenta MQTT mientras la bocina está activa.
 * - Drena mqttQueue con presupuesto (4 publishes / 50 ms) tras cada loop().
 * - Se suscribe a homeassistant/status para republicar discovery al volver HA.
 */

#include <ESP8266WiFi.h>
#include "mqtt_manager.h"
#include "mqtt_publish_queue.h"
#include "hal.h"
#include "config.h"
#include "logger.h"
#include "mqtt_discovery.h"

extern Buzzer buzzer;
extern String modoAlarma;

WiFiClient espClient;
PubSubClient mqtt(espClient);
bool mqttDisponible = false;
ModoMQTT modoMQTT = ModoMQTT::MODO_LOCAL;

static unsigned long ultimoIntentoMQTT = 0;
static unsigned long ultimoUptime = 0;
static unsigned long ultimoSondeo = 0;
static uint8_t fallosConsecutivos = 0;
static unsigned long inicioMQTT = 0;
static bool primerIntentoPendiente = false;
static bool solicitarStateSync = false;
static const unsigned long MQTT_INITIAL_DELAY_MS = 1000;

const char* modoMQTTStr() {
    return (modoMQTT == ModoMQTT::MODO_HA) ? "HA" : "LOCAL";
}

static void publicarModoAlarma() {
    mqtt.publish(TOPIC_MODO_STATE, modoAlarma.c_str(), true);
    mqtt.publish(TOPIC_V3_MODO_STATE, modoAlarma.c_str(), true);
}

static void procesarComandoBocina(const String& mensaje) {
    if (mensaje == "ON") {
        buzzer.timedOn(DURACION_BOCINA_MOTION_MS);
    } else if (mensaje == "OFF") {
        buzzer.off();
        publicarEstadoBocina();
    }
}

static void procesarComandoModo(const String& mensaje) {
    if (mensaje != "armado" && mensaje != "desarmado") return;
    modoAlarma = mensaje;
    publicarModoAlarma();
}

static void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String mensaje;
    for (unsigned int i = 0; i < length; i++) mensaje += (char)payload[i];

    String t = String(topic);
    LOG_INFO("MQTT [%s]: %s", topic, mensaje.c_str());

    // Los topics V3/V4 son aliases de compatibilidad. Ambos pasan por el
    // mismo handler; aún no existe CMD_ID/deduplicación MQTT contractual.
    if (t == TOPIC_BOCINA_CMD || t == TOPIC_V3_BOCINA_CMD) {
        procesarComandoBocina(mensaje);
    } else if (t == TOPIC_MODO || t == TOPIC_V3_MODO) {
        procesarComandoModo(mensaje);
    } else if (t == "homeassistant/status" && mensaje == "online") {
        LOG_INFO("HA reinicio; republicando discovery");
        publicarDiscovery();
    }
}

static bool intentarConexionMQTT() {
    LOG_INFO("MQTT: conectando a %s:%d...", mqtt_server, mqtt_port);

    bool ok;
    if (mqtt_user[0] != '\0') {
        ok = mqtt.connect(mqtt_client_id, mqtt_user, mqtt_pass,
                          TOPIC_V3_ESTADO, 0, true, "offline");
    } else {
        ok = mqtt.connect(mqtt_client_id, TOPIC_V3_ESTADO, 0, true, "offline");
    }

    if (ok) {
        LOG_INFO("MQTT conectado OK");
        mqttDisponible = true;
        solicitarStateSync = true;
        fallosConsecutivos = 0;

        // El topic V3 es el LWT/availability que consume Discovery.
        mqtt.publish(TOPIC_V3_ESTADO, "online", true);
        mqtt.publish(TOPIC_ESTADO, "online", true);
        publicarDiscovery();

        const char* buzzerState = buzzer.isOn() ? "ON" : "OFF";
        mqtt.publish(TOPIC_BOCINA_STATE, buzzerState, true);
        mqtt.publish(TOPIC_V3_BOCINA_STATE, buzzerState, true);
        mqtt.publish(TOPIC_MODO_STATE, modoAlarma.c_str(), true);
        mqtt.publish(TOPIC_V3_MODO_STATE, modoAlarma.c_str(), true);
        const String ip = WiFi.localIP().toString();
        mqtt.publish(TOPIC_IP, ip.c_str(), true);
        mqtt.publish(TOPIC_V3_IP, ip.c_str(), true);

        mqtt.subscribe(TOPIC_BOCINA_CMD);
        mqtt.subscribe(TOPIC_V3_BOCINA_CMD);
        mqtt.subscribe(TOPIC_MODO);
        mqtt.subscribe(TOPIC_V3_MODO);
        // HA republica "online" en homeassistant/status tras su arranque.
        mqtt.subscribe("homeassistant/status");
        return true;
    }

    LOG_WARN("MQTT fallo, rc=%d", mqtt.state());
    mqttDisponible = false;
    fallosConsecutivos++;
    return false;
}

void inicializarMQTT() {
    mqtt.setSocketTimeout(2);
    mqtt.setServer(mqtt_server, mqtt_port);
    mqtt.setBufferSize(768);
    mqtt.setCallback(mqttCallback);

    // No conectar desde setup: IoTNode debe quedar atendiendo UDP primero.
    modoMQTT = ModoMQTT::MODO_LOCAL;
    mqttDisponible = false;
    inicioMQTT = millis();
    primerIntentoPendiente = true;
    ultimoSondeo = inicioMQTT;
    LOG_INFO(">>> MQTT diferido: arranque en modo LOCAL <<<");
}

void manejarMQTT() {
    if (WiFi.status() != WL_CONNECTED) {
        mqttDisponible = false;
        return;
    }

    const unsigned long ahora = millis();
    if (primerIntentoPendiente && ahora - inicioMQTT >= MQTT_INITIAL_DELAY_MS) {
        // Igual que V3: una alarma local activa tiene prioridad sobre MQTT.
        if (buzzer.isOn()) return;
        primerIntentoPendiente = false;
        LOG_INFO("Boot: probando broker MQTT tras arranque IoT...");
        if (intentarConexionMQTT()) {
            modoMQTT = ModoMQTT::MODO_HA;
            LOG_INFO(">>> Modo HA <<<");
        } else {
            modoMQTT = ModoMQTT::MODO_LOCAL;
            ultimoSondeo = ahora;
            LOG_INFO(">>> Modo LOCAL (broker no disponible) <<<");
        }
        return;
    }

    // --- MODO LOCAL ---
    if (modoMQTT == ModoMQTT::MODO_LOCAL) {
        const unsigned long localAhora = millis();
        if (localAhora - ultimoSondeo >= MQTT_SONDEO_INTERVAL_MS) {
            ultimoSondeo = localAhora;
            if (!buzzer.isOn()) {
                LOG_INFO("Sondeo broker...");
                if (intentarConexionMQTT()) {
                    modoMQTT = ModoMQTT::MODO_HA;
                    LOG_INFO(">>> Broker detectado! Modo HA <<<");
                }
            } else {
                // Posponer sondeo 30s si hay alarma activa, como en V3.
                ultimoSondeo = localAhora - MQTT_SONDEO_INTERVAL_MS + 30000;
            }
        }
        return;
    }

    // --- MODO HA ---
    if (!mqtt.connected()) {
        if (mqttDisponible) {
            // Sellar el instante de la caída una sola vez.
            ultimoIntentoMQTT = millis();
        }
        mqttDisponible = false;
        const unsigned long reconnectAhora = millis();
        if (reconnectAhora - ultimoIntentoMQTT > MQTT_RECONNECT_INTERVAL_MS) {
            ultimoIntentoMQTT = reconnectAhora;
            if (buzzer.isOn()) return;
            if (!intentarConexionMQTT()) {
                if (fallosConsecutivos >= 3) {
                    LOG_WARN("Broker caido, volviendo a LOCAL");
                    modoMQTT = ModoMQTT::MODO_LOCAL;
                    ultimoSondeo = millis() - MQTT_SONDEO_INTERVAL_MS +
                                   MQTT_SONDEO_DESPUES_DE_CAIDA_MS;
                    fallosConsecutivos = 0;
                }
            }
        }
        return;
    }

    const bool loopOk = mqtt.loop();
    mqttDisponible = loopOk && mqtt.connected();
    if (!mqttDisponible) return;

    const unsigned long uptimeAhora = millis();
    if (uptimeAhora - ultimoUptime > 60000) {
        ultimoUptime = uptimeAhora;
        const String uptime = String(millis() / 1000);
        mqtt.publish(TOPIC_UPTIME, uptime.c_str(), true);
        mqtt.publish(TOPIC_V3_UPTIME, uptime.c_str(), true);
    }

    // Drenar la cola de publicación con presupuesto acotado.
    // El orden de llamada en main.cpp garantiza que node.loop() y
    // buzzer.loop() ya corrieron esta iteración, así que este bloque no
    // puede retrasar la recepción UDP ni la activación de la bocina.
    constexpr uint8_t  MAX_PUBLISHES = 4;
    constexpr uint32_t MAX_MS        = 50;
    const uint32_t t0 = millis();
    uint8_t budget = MAX_PUBLISHES;
    PendingPublish item;
    while (budget > 0 && (millis() - t0) < MAX_MS && mqttQueue.pop(item)) {
        budget--;
        if (!mqtt.connected()) {
            if (item.retained) mqttQueue.requeue(item);
            break;
        }
        if (mqtt.publish(item.topic, item.payload, item.retained)) {
            mqttQueue.markPublished();
        } else {
            LOG_WARN("MQTT publish fallo: %s", item.topic);
            if (item.retained) mqttQueue.requeue(item);
        }
    }
}

bool consumirSolicitudStateSync() {
    const bool pendiente = solicitarStateSync;
    solicitarStateSync = false;
    return pendiente;
}

void publicarEstadoBocina() {
    if (mqtt.connected()) {
        const char* state = buzzer.isOn() ? "ON" : "OFF";
        mqtt.publish(TOPIC_BOCINA_STATE, state, true);
        mqtt.publish(TOPIC_V3_BOCINA_STATE, state, true);
    }
}
