/**
 * IoTNode V4.3 — Nodo de comunicación IoTProtocol
 *
 * Cambios vs V4.0:
 * - FIFO real con head/tail/count (no tabla de slots)
 * - 3 prioridades: URGENT > NORMAL > BACKGROUND
 * - Un solo paquete reliable en vuelo a la vez (simplifica ACK tracking)
 * - Buffering sin WiFi: encolar aunque no haya conexión, enviar cuando vuelva
 * - Deduplicación con BOOT_ID + ventana deslizante de SEQ (incluye replay antiguo)
 * - ACKs ligados a la sesión BOOT_ID conocida del remoto
 * - Recepción UDP acotada por iteración para no monopolizar loop()
 * - Política de overflow: URGENT nunca se descarta, BACKGROUND sí
 * - BOOT_ID proporcionado por el firmware o generado aleatoriamente como fallback
 *
 * Uso:
 *   IoTNode node(MY_DEVICE_ID, UDP_PORT);
 *   node.begin();
 *   node.sendEvent(eventCode, destIP, destPort);
 *   node.loop();  // cada iteración
 */

#pragma once
#include "IoTProtocol.h"
#include <WiFiUdp.h>

// ============================================================
// Configuración
// ============================================================

#define IOT_QUEUE_SIZE        8     // Capacidad de la cola FIFO
#define IOT_MAX_REMOTES       8     // Tabla de dispositivos remotos
#define IOT_MAX_RETRIES       5     // Reintentos para reliable
#define IOT_DEDUP_WINDOW      8     // Ventana de deduplicación (últimos N SEQ por remoto)
#define IOT_MAX_RX_PER_LOOP   8     // Máximo de datagramas UDP procesados por loop()

// Timeouts (ms)
#define IOT_ACK_TIMEOUT_BASE    300
#define IOT_ACK_TIMEOUT_MAX     2000
#define IOT_HEARTBEAT_INTERVAL  60000   // 60s default
#define IOT_STALE_TIMEOUT_MS    90000   // 90s sin paquete → STALE
#define IOT_OFFLINE_TIMEOUT_MS  180000  // 180s sin paquete → OFFLINE

// ============================================================
// Política de overflow de cola
// ============================================================

enum class QueueOverflow : uint8_t {
    DROP_NEWEST,     // Rechazar el nuevo paquete
    DROP_OLDEST_BG,  // Descartar el BACKGROUND más viejo (default)
    DROP_OLDEST,     // Descartar el más viejo sin importar prioridad
};

// ============================================================
// Callback
// ============================================================

typedef void (*IoTPacketHandler)(const IoTPacket &pkt, IPAddress remoteIP, uint16_t remotePort);

// ============================================================
// Proveedor de autenticación
// ============================================================

/**
 * Política de autenticación aplicada en la frontera de IoTNode.
 * DISABLED es un bypass completo y no invoca callbacks.
 */
enum class IoTAuthMode : uint8_t {
    DISABLED = 0,
    OPTIONAL = 1,  // Paquetes sin auth pasan; los marcados deben verificarse.
    REQUIRED = 2,  // Todo paquete entrante y saliente debe estar autenticado.
};

typedef bool (*IoTAuthVerifyCallback)(const IoTPacket &pkt, void *context);
typedef bool (*IoTAuthSignCallback)(IoTPacket &pkt, void *context);
typedef void (*IoTAuthRejectedHandler)(const IoTPacket &pkt,
                                       IPAddress remoteIP,
                                       uint16_t remotePort);

/**
 * Adaptador de autenticación sin acoplar IoTNode a HMAC, BearSSL u otro
 * proveedor. El callback de firma debe ser idempotente para permitir que
 * callers antiguos entreguen paquetes ya firmados sin doble firma.
 */
struct IoTAuthProvider {
    IoTAuthMode mode;
    bool signOutgoing;
    IoTAuthVerifyCallback verify;
    IoTAuthSignCallback sign;
    IoTAuthRejectedHandler onRejected;
    void *context;
};

// ============================================================
// Estado de conexión de un dispositivo remoto
// ============================================================

enum class DeviceState : uint8_t {
    UNKNOWN     = 0,
    ONLINE      = 1,   // Último paquete < 90s
    STALE       = 2,   // Último paquete 90–180s
    OFFLINE     = 3,   // Último paquete > 180s
};

// ============================================================
// Resultado de inyección por canal no-UDP (ej. HTTP de emergencia)
// ============================================================

enum class InjectResult : uint8_t {
    ACCEPTED  = 0,  // Evento nuevo, despachado al handler
    DUPLICATE = 1,  // Ya visto en la ventana (src, bootId, seq)
    REJECTED  = 2,  // Sesión inválida, replay fuera de ventana o error
};

// ============================================================
// Dispositivo remoto conocido (registry + deduplicación)
// ============================================================

struct RemoteDevice {
    uint8_t   id;
    IPAddress ip;
    uint16_t  port;

    // Deduplicación: ventana de últimos N SEQ procesados
    uint16_t  bootId;
    uint32_t  seqWindow[IOT_DEDUP_WINDOW];
    uint8_t   seqWindowCount;   // Cuántos hay en la ventana (0–IOT_DEDUP_WINDOW)
    uint8_t   seqWindowHead;    // Índice circular para insertar el siguiente
    uint32_t  seqHighest;       // Mayor SEQ aceptado en la sesión actual
    uint8_t   seqBitmap;        // Bit 0=seqHighest; bits siguientes=SEQ anteriores

    // Registry (se llena con HELLO)
    uint8_t   deviceType;
    char       name[IOT_DEVICE_NAME_MAX];
    char       fwVersion[12];

    // Estado de vida
    unsigned long lastSeen;       // millis() del último paquete recibido
    unsigned long lastHeartbeat;  // millis() del último HEARTBEAT
    DeviceState   state;
    bool          active;         // Slot en uso
};

// ============================================================
// Entrada en la cola FIFO
// ============================================================

struct QueueEntry {
    IoTPacket pkt;
    IPAddress destIP;
    uint16_t  destPort;
    Priority  priority;
    bool      occupied;    // Slot está en uso
};

// ============================================================
// Estado del canal reliable (1 solo paquete en vuelo)
// ============================================================

struct ReliableChannel {
    IoTPacket pkt;
    IPAddress destIP;
    uint16_t  destPort;
    uint8_t   attempt;        // Intento actual (0-based)
    uint8_t   maxAttempts;
    unsigned long nextRetryAt; // millis() del próximo envío/reintento
    unsigned long sentAt;      // millis() cuando se envió (para RTT)
    uint16_t expectedBootId;   // Sesión del remoto conocida al iniciar este reliable
    bool      expectedBootKnown;
    bool      active;          // Hay algo en vuelo
    bool      waitingAck;      // Esperando ACK
};

// ============================================================
// Estadísticas internas (IoTStats)
// ============================================================

struct IoTStats {
    uint32_t txPackets;       // Total paquetes enviados (incluyendo reintentos)
    uint32_t rxPackets;       // Total paquetes recibidos válidos
    uint32_t txReliable;      // Paquetes reliable iniciados
    uint32_t ackReceived;     // ACKs recibidos exitosos
    uint32_t ackTimeouts;     // Reliable que fallaron (sin ACK tras N intentos)
    uint32_t retries;         // Reintentos individuales
    uint32_t duplicates;      // Paquetes duplicados descartados
    uint32_t replays;         // Paquetes fuera de sesión/ventana descartados
    uint32_t crcErrors;       // Paquetes con CRC inválido (contados en deserialize)
    uint32_t queueDrops;      // Eventos no encolados (cola llena)
    uint32_t queueOverflows;  // Eventos que desplazaron otro (overflow policy)
};

// ============================================================
// RTT (Round-Trip Time) del canal reliable
// ============================================================

struct IoTRtt {
    uint32_t lastMs;    // Último RTT medido (ms)
    uint32_t minMs;     // Mínimo histórico
    uint32_t maxMs;     // Máximo histórico
    uint32_t avgMs;     // Promedio móvil (EMA)
    uint32_t samples;   // Cantidad de mediciones
};

// ============================================================
// IoTNode — Clase principal V4.1
// ============================================================

class IoTNode {
public:
    IoTNode(uint8_t deviceId, uint16_t udpPort);

    // --- Inicialización ---
    // Genera un BOOT_ID aleatorio para consumidores legacy.
    void begin();
    // Usa un BOOT_ID persistente proporcionado por el firmware.
    void begin(uint16_t bootId);

    // --- Loop (llamar cada iteración) ---
    void loop();

    // --- Envío de alto nivel ---

    /**
     * Encolar un evento simple. ACK_REQUIRED + RELIABLE por defecto.
     * Funciona aunque no haya WiFi — se envía cuando conecte.
     */
    bool sendEvent(uint8_t eventCode, IPAddress destIP, uint16_t destPort,
                   uint8_t destId = IOT_DEVICE_CENTRAL);

    /**
     * Encolar un paquete genérico pre-armado.
     * La prioridad se deduce de pkt.flags.
     */
    bool enqueue(IoTPacket &pkt, IPAddress destIP, uint16_t destPort);

    /**
     * Envío directo UDP (sin cola, sin ACK). Para ACKs y heartbeats.
     */
    void sendDirect(const IoTPacket &pkt, IPAddress destIP, uint16_t destPort);

    /**
     * Inyecta un evento que NO llegó por UDP, reutilizando el pipeline de
     * recepción de IoTNode: validación de sesión BOOT_ID, deduplicación por
     * ventana (src, bootId, seq) y dispatch al handler registrado.
     *
     * Está pensado para un canal de respaldo (p. ej. el endpoint HTTP de
     * emergencia de la central cuando un satélite ESPHome pierde el broker).
     *
     * NO re-verifica autenticación: el payload JSON de ese canal no transporta
     * el HMAC del wire binario. El caller (http_endpoint) decide si el canal
     * está autorizado antes de llamar aquí. Tampoco envía ACK por UDP: el ACK
     * del canal de respaldo es la propia respuesta HTTP (200/409/403).
     *
     * El mapeo con el pipeline UDP real:
     *  - bootId == 0 se rechaza igual que en _processIncoming (sentinel interno).
     *  - _updateRemote(touch=false) crea/avanza la sesión sin marcar liveness.
     *  - _isDuplicate separa, vía contadores de stats, un duplicado exacto
     *    (DUPLICATE) de un replay fuera de ventana o sesión vieja (REJECTED).
     *  - solo si se acepta se llama a _markRemoteSeen + handler.
     *
     * @return ACCEPTED  evento nuevo, despachado al handler
     *         DUPLICATE mismo (src, bootId, seq) ya visto en la ventana
     *         REJECTED  bootId inválido, sesión antigua o replay fuera de ventana
     */
    InjectResult injectEvent(uint8_t src, uint16_t bootId, uint32_t seq,
                             uint8_t eventCode, uint8_t value,
                             IPAddress remoteIP);

    // --- Heartbeat ---
    void enableHeartbeat(IPAddress destIP, uint16_t destPort,
                         unsigned long intervalMs = IOT_HEARTBEAT_INTERVAL);
    void disableHeartbeat();

    // --- Discovery ---
    void sendHello(IPAddress destIP, uint16_t destPort, uint8_t deviceType,
                   const char* devName);

    // --- Diagnóstico opcional de aplicación ---
    // Los TLV de versión y reset se omiten si no se configuran, preservando
    // callers legacy y permitiendo que el core siga siendo reutilizable.
    void setFirmwareVersion(const char* version);
    void setBootReason(BootReason reason);

    // --- Callback ---
    void onPacketReceived(IoTPacketHandler handler);

    // --- Autenticación ---
    // Copia el proveedor; puede reemplazarse en runtime tras un cambio de config.
    void setAuthProvider(const IoTAuthProvider &provider);
    void clearAuthProvider();
    IoTAuthMode getAuthMode() const { return _authProvider.mode; }

    // --- Configuración ---
    void setOverflowPolicy(QueueOverflow policy);

    // --- Info ---
    uint8_t  getDeviceId() const { return _deviceId; }
    uint16_t getBootId() const   { return _bootId; }
    uint32_t getNextSeq();
    uint8_t  queuedCount() const;
    bool     isQueueEmpty() const;
    bool     isQueueFull() const;
    bool     isReliableInFlight() const { return _reliable.active; }

    // --- Estadísticas ---
    const IoTStats& getStats() const { return _stats; }
    const IoTRtt& getRtt() const { return _rtt; }
    void resetStats();

    // --- Remotos ---
    RemoteDevice* getRemote(uint8_t id);
    // Registra solo endpoint; bootId=0 es sentinel interno, no sesión wire.
    void registerRemote(uint8_t id, IPAddress ip, uint16_t port);
    uint8_t getRemoteCount() const;
    void updateDeviceStates();  // Llamar periódicamente para ONLINE→STALE→OFFLINE

private:
    uint8_t  _deviceId;
    uint16_t _udpPort;
    uint16_t _bootId;      // Persistente si lo proporciona el firmware; aleatorio como fallback
    uint32_t _seq;
    WiFiUDP _udp;

    // Cola FIFO
    QueueEntry _queue[IOT_QUEUE_SIZE];
    uint8_t    _queueCount;
    QueueOverflow _overflowPolicy;

    // Canal reliable (1 paquete en vuelo)
    ReliableChannel _reliable;

    // Estadísticas
    IoTStats _stats;
    IoTRtt   _rtt;

    // Dispositivos remotos
    RemoteDevice _remotes[IOT_MAX_REMOTES];

    // Heartbeat
    bool _hbEnabled;
    IPAddress _hbIP;
    uint16_t _hbPort;
    unsigned long _hbInterval;
    unsigned long _lastHb;

    // Device state tracking
    unsigned long _lastStateCheck;

    // Callback
    IoTPacketHandler _handler;

    // Proveedor de autenticación (DISABLED por defecto para compatibilidad)
    IoTAuthProvider _authProvider;

    // Diagnóstico opcional aportado por el firmware de aplicación.
    const char* _firmwareVersion;
    BootReason _bootReason;

    // Buffer de recepción
    uint8_t _rxBuf[IOT_MAX_PACKET];

    // --- Internos ---
    void _processIncoming();
    void _processQueue();
    void _processReliable();
    void _sendHeartbeat();
    void _updateDeviceStates();
    bool _handleAck(const IoTPacket &pkt, IPAddress remoteIP, uint16_t remotePort);
    void _sendAutoAck(const IoTPacket &pkt, IPAddress remoteIP, uint16_t remotePort);
    bool _isDuplicate(uint8_t srcId, uint16_t bootId, uint32_t seq);
    bool _updateRemote(uint8_t srcId, IPAddress ip, uint16_t port,
                       uint16_t bootId, uint32_t seq,
                       bool *sessionChanged = nullptr, bool touch = true);
    void _markRemoteSeen(uint8_t srcId, IPAddress ip, uint16_t port);
    void _resetRemoteDedup(RemoteDevice &dev, uint16_t bootId);
    void _fillRemoteFromHello(RemoteDevice &dev, const IoTPacket &pkt);
    void _transmitPacket(const IoTPacket &pkt, IPAddress destIP, uint16_t destPort);
    bool _verifyIncoming(const IoTPacket &pkt, IPAddress remoteIP, uint16_t remotePort);
    bool _prepareOutgoing(IoTPacket &pkt);
    int  _findHighestPriorityEntry() const;
    int  _findLowestPriorityEntry() const;
    unsigned long _calcBackoff(uint8_t attempt);
};
