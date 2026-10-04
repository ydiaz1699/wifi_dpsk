/**
 * IoTNode V4.3 — Implementación con hardening
 *
 * Fixes vs V4.1:
 * - Dedup Window: ventana circular de últimos 8 SEQ por dispositivo
 * - ACK siempre se envía (incluso para duplicados)
 * - Device Registry ampliado (type, name, fw, state)
 * - ONLINE/STALE/OFFLINE state machine
 * - _fillRemoteFromHello() para registrar info de HELLO
 */

#include "IoTNode.h"
#include <ESP8266WiFi.h>
#include <string.h>

namespace {

bool addTlvChecked(bool added, const char* context,
                   const char* tagName) {
    if (!added) {
        Serial.printf("[W] %s: TLV %s descartado por falta de payload\n",
                      context, tagName);
    }
    return added;
}

}  // namespace

// ============================================================
// Constructor + begin
// ============================================================

IoTNode::IoTNode(uint8_t deviceId, uint16_t udpPort)
    : _deviceId(deviceId), _udpPort(udpPort), _bootId(0), _seq(0),
      _queueCount(0), _overflowPolicy(QueueOverflow::DROP_OLDEST_BG),
      _hbEnabled(false), _hbInterval(IOT_HEARTBEAT_INTERVAL),
      _lastHb(0), _lastStateCheck(0), _handler(nullptr),
      _firmwareVersion(nullptr), _bootReason(BootReason::UNKNOWN)
{
    memset(_queue, 0, sizeof(_queue));
    memset(_remotes, 0, sizeof(_remotes));
    memset(&_reliable, 0, sizeof(_reliable));
    memset(&_stats, 0, sizeof(_stats));
    memset(&_rtt, 0, sizeof(_rtt));
    memset(&_authProvider, 0, sizeof(_authProvider));
    _authProvider.mode = IoTAuthMode::DISABLED;
    _rtt.minMs = 0xFFFFFFFF;  // Inicializar min alto
}

void IoTNode::begin() {
    randomSeed(analogRead(A0) ^ micros());
    begin((uint16_t)random(1, 65535));
}

void IoTNode::begin(uint16_t bootId) {
    // BOOT_ID cero está reservado; normalizarlo también protege el fallback.
    _bootId = bootId == 0 ? 1 : bootId;
    _seq = 0;
    _lastStateCheck = millis();
    _udp.begin(_udpPort);
}

// ============================================================
// Loop
// ============================================================

void IoTNode::loop() {
    _processIncoming();
    _processReliable();
    _processQueue();
    if (_hbEnabled) _sendHeartbeat();

    // Actualizar estados cada 10 segundos
    unsigned long ahora = millis();
    if (ahora - _lastStateCheck >= 10000) {
        _lastStateCheck = ahora;
        _updateDeviceStates();
    }
}

// ============================================================
// Transmisión UDP
// ============================================================

void IoTNode::_transmitPacket(const IoTPacket &pkt, IPAddress destIP, uint16_t destPort) {
    uint8_t buf[IOT_MAX_PACKET];
    size_t len = iot_serialize(pkt, buf, sizeof(buf));
    if (len > 0) {
        _udp.beginPacket(destIP, destPort);
        _udp.write(buf, len);
        _udp.endPacket();
        _stats.txPackets++;
    }
}

void IoTNode::sendDirect(const IoTPacket &pkt, IPAddress destIP, uint16_t destPort) {
    if (WiFi.status() != WL_CONNECTED) return;

    // Copia para conservar la API const y firmar antes de serializar. El
    // proveedor debe hacer sign idempotente para callers legacy ya firmados.
    IoTPacket prepared = pkt;
    if (!_prepareOutgoing(prepared)) return;
    _transmitPacket(prepared, destIP, destPort);
}

// ============================================================
// Inyección por canal no-UDP (p. ej. HTTP de emergencia)
//
// Replica el orden de _processIncoming() para un EVENT: rechaza bootId=0,
// actualiza sesión sin tocar liveness, deduplica y solo entonces despacha.
// La distinción DUPLICATE vs REJECTED se obtiene observando qué contador
// de stats incrementó _isDuplicate, porque esa función fusiona ambos casos
// en un único retorno booleano.
// ============================================================

InjectResult IoTNode::injectEvent(uint8_t src, uint16_t bootId, uint32_t seq,
                                  uint8_t eventCode, uint8_t value,
                                  IPAddress remoteIP) {
    // bootId=0 es sentinel interno de registerRemote(), nunca una sesión wire.
    // _processIncoming() lo descarta explícitamente; aquí hacemos lo mismo.
    if (bootId == 0) {
        _stats.replays++;
        return InjectResult::REJECTED;
    }

    _stats.rxPackets++;

    // Avanzar/crear la sesión sin marcar liveness todavía (touch=false),
    // igual que el camino UDP: un replay antiguo no debe mover el registry.
    bool sessionChanged = false;
    if (!_updateRemote(src, remoteIP, 0, bootId, seq, &sessionChanged, false)) {
        _stats.replays++;
        return InjectResult::REJECTED;
    }

    // _isDuplicate devuelve true tanto para duplicado exacto como para replay
    // fuera de ventana / sesión no coincidente. Diferenciamos leyendo qué
    // contador movió: _stats.duplicates => DUPLICATE; _stats.replays => REJECTED.
    const uint32_t dupBefore    = _stats.duplicates;
    const uint32_t replayBefore = _stats.replays;
    if (_isDuplicate(src, bootId, seq)) {
        if (_stats.duplicates != dupBefore) return InjectResult::DUPLICATE;
        if (_stats.replays != replayBefore) return InjectResult::REJECTED;
        // Salvaguarda: si por algún motivo no movió contador, trátalo como
        // rechazo conservador en vez de despachar un evento dudoso.
        return InjectResult::REJECTED;
    }

    // Aceptado: ahora sí marcamos liveness y construimos el EVENT para el
    // handler, con el mismo contrato TLV que un EVENT llegado por UDP.
    _markRemoteSeen(src, remoteIP, 0);

    IoTPacket pkt;
    pkt.version = IOT_PROTOCOL_VER;
    pkt.type = MsgType::EVENT;
    pkt.src = src;
    pkt.dst = _deviceId;
    pkt.bootId = bootId;
    pkt.seq = seq;
    pkt.flags = IOT_FLAG_RELIABLE;  // informativo; este canal no reintenta por UDP
    pkt.clearPayload();
    pkt.addTLV_uint8(TlvTag::EVENT_TYPE, eventCode);
    pkt.addTLV_uint8(TlvTag::EVENT_VALUE, value);

    if (_handler) {
        _handler(pkt, remoteIP, 0);
    }
    return InjectResult::ACCEPTED;
}

// ============================================================
// Canal reliable
// ============================================================

void IoTNode::_processReliable() {
    if (!_reliable.active) return;
    unsigned long ahora = millis();

    if (_reliable.waitingAck && ahora < _reliable.nextRetryAt) return;

    if (_reliable.waitingAck) {
        _reliable.attempt++;
        _stats.retries++;
        if (_reliable.attempt >= _reliable.maxAttempts) {
            _reliable.active = false;
            _stats.ackTimeouts++;
            return;
        }
    }

    if (WiFi.status() == WL_CONNECTED) {
        _transmitPacket(_reliable.pkt, _reliable.destIP, _reliable.destPort);
        _reliable.waitingAck = true;
        _reliable.sentAt = ahora;
        _reliable.nextRetryAt = ahora + _calcBackoff(_reliable.attempt);
    } else {
        _reliable.nextRetryAt = ahora + 1000;
    }
}

unsigned long IoTNode::_calcBackoff(uint8_t attempt) {
    unsigned long ms = IOT_ACK_TIMEOUT_BASE;
    for (uint8_t i = 0; i < attempt; i++) {
        ms = (ms * 3) / 2;
        if (ms > IOT_ACK_TIMEOUT_MAX) { ms = IOT_ACK_TIMEOUT_MAX; break; }
    }
    return ms;
}

// ============================================================
// Cola FIFO con prioridades
// ============================================================

void IoTNode::_processQueue() {
    if (_reliable.active) return;
    if (_queueCount == 0) return;

    int idx = _findHighestPriorityEntry();
    if (idx < 0) return;

    QueueEntry &entry = _queue[idx];

    if (entry.pkt.isReliable()) {
        _reliable.pkt = entry.pkt;
        _reliable.destIP = entry.destIP;
        _reliable.destPort = entry.destPort;
        _reliable.attempt = 0;
        _reliable.maxAttempts = IOT_MAX_RETRIES;
        _reliable.nextRetryAt = 0;
        _reliable.sentAt = 0;
        _reliable.expectedBootId = 0;
        _reliable.expectedBootKnown = false;
        RemoteDevice *remote = getRemote(entry.pkt.dst);
        if (remote && remote->bootId != 0) {
            _reliable.expectedBootId = remote->bootId;
            _reliable.expectedBootKnown = true;
        }
        _reliable.active = true;
        _reliable.waitingAck = false;
        _stats.txReliable++;
    } else {
        if (WiFi.status() == WL_CONNECTED) {
            _transmitPacket(entry.pkt, entry.destIP, entry.destPort);
        }
    }

    entry.occupied = false;
    _queueCount--;
}

uint32_t IoTNode::getNextSeq() {
    _seq++;
    if (_seq == 0) _seq = 1;
    return _seq;
}

uint8_t IoTNode::queuedCount() const { return _queueCount; }
bool IoTNode::isQueueEmpty() const { return _queueCount == 0; }
bool IoTNode::isQueueFull() const { return _queueCount >= IOT_QUEUE_SIZE; }

int IoTNode::_findHighestPriorityEntry() const {
    int best = -1;
    Priority bestPri = Priority::BACKGROUND;
    for (int i = 0; i < IOT_QUEUE_SIZE; i++) {
        if (!_queue[i].occupied) continue;
        if (best == -1 || _queue[i].priority < bestPri) {
            best = i; bestPri = _queue[i].priority;
        }
    }
    return best;
}

int IoTNode::_findLowestPriorityEntry() const {
    int worst = -1;
    Priority worstPri = Priority::URGENT;
    for (int i = 0; i < IOT_QUEUE_SIZE; i++) {
        if (!_queue[i].occupied) continue;
        if (worst == -1 || _queue[i].priority > worstPri) {
            worst = i; worstPri = _queue[i].priority;
        }
    }
    return worst;
}

bool IoTNode::enqueue(IoTPacket &pkt, IPAddress destIP, uint16_t destPort) {
    IoTPacket prepared = pkt;
    if (!_prepareOutgoing(prepared)) return false;
    Priority pri = prepared.priority();

    if (!isQueueFull()) {
        for (int i = 0; i < IOT_QUEUE_SIZE; i++) {
            if (!_queue[i].occupied) {
                _queue[i].pkt = prepared;
                _queue[i].destIP = destIP;
                _queue[i].destPort = destPort;
                _queue[i].priority = pri;
                _queue[i].occupied = true;
                _queueCount++;
                return true;
            }
        }
    }

    switch (_overflowPolicy) {
        case QueueOverflow::DROP_NEWEST:
            _stats.queueDrops++;
            return false;
        case QueueOverflow::DROP_OLDEST_BG: {
            if (pri == Priority::BACKGROUND) { _stats.queueDrops++; return false; }
            int victim = -1;
            for (int i = 0; i < IOT_QUEUE_SIZE; i++) {
                if (_queue[i].occupied && _queue[i].priority == Priority::BACKGROUND) {
                    victim = i; break;
                }
            }
            if (victim < 0) { _stats.queueDrops++; return false; }
            _queue[victim].pkt = prepared;
            _queue[victim].destIP = destIP;
            _queue[victim].destPort = destPort;
            _queue[victim].priority = pri;
            _stats.queueOverflows++;
            return true;
        }
        case QueueOverflow::DROP_OLDEST: {
            int victim = _findLowestPriorityEntry();
            if (victim < 0) { _stats.queueDrops++; return false; }
            if (_queue[victim].priority < pri) { _stats.queueDrops++; return false; }
            _queue[victim].pkt = prepared;
            _queue[victim].destIP = destIP;
            _queue[victim].destPort = destPort;
            _queue[victim].priority = pri;
            _stats.queueOverflows++;
            return true;
        }
    }
    _stats.queueDrops++;
    return false;
}

void IoTNode::setOverflowPolicy(QueueOverflow policy) {
    _overflowPolicy = policy;
}

// ============================================================
// Recepción — drain acotado, ACK antes de dedup
// ============================================================

void IoTNode::_processIncoming() {
    for (uint8_t processed = 0; processed < IOT_MAX_RX_PER_LOOP; processed++) {
        int packetSize = _udp.parsePacket();
        if (!packetSize) break;
        if (packetSize > (int)sizeof(_rxBuf)) {
            _udp.flush();
            continue;
        }

        int len = _udp.read(_rxBuf, sizeof(_rxBuf));
        if (len <= 0) continue;

        IPAddress remoteIP = _udp.remoteIP();
        uint16_t remotePort = _udp.remotePort();

        IoTPacket pkt;
        if (!iot_deserialize(_rxBuf, len, pkt)) continue;
        if (pkt.dst != _deviceId && pkt.dst != IOT_DEVICE_BROADCAST) continue;

        // La autenticación es la primera operación con efectos potenciales.
        // Un rechazo no puede ocupar/actualizar registry, enviar ACK,
        // deduplicar ni llegar al handler de aplicación.
        if (!_verifyIncoming(pkt, remoteIP, remotePort)) continue;

        // BOOT_ID=0 solo existe como sentinel interno de registerRemote();
        // nunca es una sesión válida en el wire. Rechazarlo aquí evita que
        // un paquete no-ACK cree registry, dedup o liveness con sesión cero.
        if (pkt.bootId == 0) {
            _stats.replays++;
            continue;
        }

        _stats.rxPackets++;

        // Un paquete autenticado puede ser reconocido aunque luego se
        // descarte por replay; esto detiene reintentos sin ejecutar efectos.
        if (pkt.type != MsgType::ACK && pkt.needsAck()) {
            _sendAutoAck(pkt, remoteIP, remotePort);
        }

        // Un ACK solo puede confirmar el reliable activo y con SRC/SEQ
        // coincidentes. En el bootstrap, cuando todavía no hay sesión del
        // remoto (o solo existe el endpoint registrado con bootId=0), el
        // propio ACK autenticado/estructural establece el primer BOOT_ID
        // remoto. Una vez conocido, la validación vuelve a ser estricta.
        if (pkt.type == MsgType::ACK) {
            if (_handleAck(pkt, remoteIP, remotePort)) {
                RemoteDevice* remote = getRemote(pkt.src);
                if (remote && remote->bootId == pkt.bootId) {
                    _markRemoteSeen(pkt.src, remoteIP, remotePort);
                }
            }
            continue;
        }

        bool sessionChanged = false;
        // No marcar liveness hasta que dedup acepte el paquete. Así, un
        // replay antiguo no puede mantener un remoto en ONLINE ni cambiar
        // su endpoint, aunque sí pueda recibir el ACK protocolario.
        if (!_updateRemote(pkt.src, remoteIP, remotePort, pkt.bootId, pkt.seq,
                           &sessionChanged, false)) {
            // Un paquete autenticado de una sesión anterior no puede mover el
            // registry hacia atrás ni alcanzar ACK/callback/dedup.
            _stats.replays++;
            continue;
        }

        // Si el remoto reinició mientras había un reliable en vuelo, su
        // primer paquete válido establece la nueva sesión para ese canal.
        if (sessionChanged && _reliable.active &&
            pkt.src == _reliable.pkt.dst && pkt.bootId != 0) {
            _reliable.expectedBootId = pkt.bootId;
            _reliable.expectedBootKnown = true;
        }

        if (pkt.isReliable() && _isDuplicate(pkt.src, pkt.bootId, pkt.seq)) {
            continue;
        }

        _markRemoteSeen(pkt.src, remoteIP, remotePort);

        if (pkt.type == MsgType::HELLO) {
            RemoteDevice* dev = getRemote(pkt.src);
            if (dev) _fillRemoteFromHello(*dev, pkt);
        }

        if (_handler) {
            _handler(pkt, remoteIP, remotePort);
        }
    }
}

// ============================================================
// ACK
// ============================================================

void IoTNode::_sendAutoAck(const IoTPacket &pkt, IPAddress remoteIP, uint16_t remotePort) {
    IoTPacket ack;
    ack.version = IOT_PROTOCOL_VER;
    ack.type = MsgType::ACK;
    ack.src = _deviceId;
    ack.dst = pkt.src;
    ack.bootId = _bootId;
    ack.seq = pkt.seq;
    ack.flags = 0;
    ack.clearPayload();
    sendDirect(ack, remoteIP, remotePort);
}

bool IoTNode::_handleAck(const IoTPacket &pkt, IPAddress remoteIP, uint16_t remotePort) {
    if (!_reliable.active || !_reliable.waitingAck) return false;
    if (pkt.seq != _reliable.pkt.seq || pkt.src != _reliable.pkt.dst) return false;

    RemoteDevice* remote = getRemote(pkt.src);
    const bool bootstrap = !_reliable.expectedBootKnown &&
                           (!remote || remote->bootId == 0);

    if (pkt.bootId == 0 ||
        (!bootstrap && (!remote || !remote->active ||
                        pkt.bootId != remote->bootId ||
                        !_reliable.expectedBootKnown ||
                        pkt.bootId != _reliable.expectedBootId))) {
        // BOOT_ID cero está reservado para registerRemote() y nunca puede
        // autenticar una sesión. Un ACK de sesión antigua, o de un remoto
        // desconocido fuera de este reliable, tampoco pasa este punto.
        _stats.replays++;
        return false;
    }

    if (bootstrap) {
        // La autenticación/estructura y la coincidencia SRC/SEQ ya fueron
        // comprobadas por _processIncoming(). Este es el único caso en que
        // un ACK puede aprender la sesión del remoto: el reliable está activo,
        // espera ACK y aún no había BOOT_ID remoto conocido.
        if (!_updateRemote(pkt.src, remoteIP, remotePort, pkt.bootId, pkt.seq,
                           nullptr, false)) {
            _stats.replays++;
            return false;
        }
        remote = getRemote(pkt.src);
        if (!remote || !remote->active || remote->bootId != pkt.bootId) {
            _stats.replays++;
            return false;
        }
        _reliable.expectedBootId = pkt.bootId;
        _reliable.expectedBootKnown = true;
    }

    _reliable.active = false;
    _stats.ackReceived++;

    if (_reliable.sentAt > 0) {
        uint32_t rtt = (uint32_t)(millis() - _reliable.sentAt);
        _rtt.lastMs = rtt;
        _rtt.samples++;
        if (rtt < _rtt.minMs) _rtt.minMs = rtt;
        if (rtt > _rtt.maxMs) _rtt.maxMs = rtt;
        if (_rtt.samples == 1) {
            _rtt.avgMs = rtt;
        } else {
            _rtt.avgMs = (_rtt.avgMs * 3 + rtt) / 4;
        }
    }
    return true;
}

// ============================================================
// Deduplicación con ventana deslizante y BOOT_ID
//
// Bit 0 representa seqHighest y cada bit siguiente un SEQ anterior. Los
// paquetes fuera de la ventana ya no pueden volver a producir un efecto.
// ============================================================

static bool seqIsNewer(uint32_t candidate, uint32_t current) {
    const uint32_t delta = candidate - current;
    return delta != 0 && delta < 0x80000000UL;
}

bool IoTNode::_isDuplicate(uint8_t srcId, uint16_t bootId, uint32_t seq) {
    RemoteDevice* dev = nullptr;
    for (int i = 0; i < IOT_MAX_REMOTES; i++) {
        if (_remotes[i].active && _remotes[i].id == srcId) {
            dev = &_remotes[i];
            break;
        }
    }
    if (!dev || dev->bootId != bootId) {
        _stats.replays++;
        return true;
    }

    if (dev->seqWindowCount == 0 || dev->seqBitmap == 0) {
        _resetRemoteDedup(*dev, bootId);
        dev->seqHighest = seq;
        dev->seqBitmap = 0x01;
        dev->seqWindow[0] = seq;
        dev->seqWindowCount = 1;
        dev->seqWindowHead = 1;
        return false;
    }

    if (seqIsNewer(seq, dev->seqHighest)) {
        const uint32_t advance = seq - dev->seqHighest;
        if (advance >= IOT_DEDUP_WINDOW) {
            dev->seqBitmap = 0;
            dev->seqWindowCount = 0;
            dev->seqWindowHead = 0;
        } else {
            dev->seqBitmap = static_cast<uint8_t>(dev->seqBitmap << advance);
        }
        dev->seqHighest = seq;
        dev->seqBitmap |= 0x01;
    } else {
        const uint32_t age = dev->seqHighest - seq;
        if (age >= IOT_DEDUP_WINDOW) {
            _stats.replays++;
            return true;
        }
        const uint8_t mask = static_cast<uint8_t>(1U << age);
        if (dev->seqBitmap & mask) {
            _stats.duplicates++;
            return true;
        }
        dev->seqBitmap |= mask;
    }

    dev->seqWindow[dev->seqWindowHead] = seq;
    dev->seqWindowHead = (dev->seqWindowHead + 1) % IOT_DEDUP_WINDOW;
    if (dev->seqWindowCount < IOT_DEDUP_WINDOW) dev->seqWindowCount++;
    return false;
}

// ============================================================
// Tabla de remotos y sesión BOOT_ID
// ============================================================

static bool bootIdIsNewer(uint16_t candidate, uint16_t current) {
    const uint16_t delta = static_cast<uint16_t>(candidate - current);
    return delta != 0 && delta < 0x8000U;
}

void IoTNode::_resetRemoteDedup(RemoteDevice &dev, uint16_t bootId) {
    dev.bootId = bootId;
    memset(dev.seqWindow, 0, sizeof(dev.seqWindow));
    dev.seqWindowCount = 0;
    dev.seqWindowHead = 0;
    dev.seqHighest = 0;
    dev.seqBitmap = 0;
}

void IoTNode::_markRemoteSeen(uint8_t srcId, IPAddress ip, uint16_t port) {
    RemoteDevice *remote = getRemote(srcId);
    if (!remote) return;
    remote->ip = ip;
    remote->port = port;
    remote->lastSeen = millis();
    remote->state = DeviceState::ONLINE;
}

bool IoTNode::_updateRemote(uint8_t srcId, IPAddress ip, uint16_t port,
                            uint16_t bootId, uint32_t seq,
                            bool *sessionChanged, bool touch) {
    if (sessionChanged) *sessionChanged = false;

    for (int i = 0; i < IOT_MAX_REMOTES; i++) {
        if (!_remotes[i].active || _remotes[i].id != srcId) continue;

        // registerRemote() usa bootId=0 para registrar solo el endpoint.
        if (bootId != 0 && _remotes[i].bootId != 0 &&
            _remotes[i].bootId != bootId) {
            if (!bootIdIsNewer(bootId, _remotes[i].bootId)) return false;
            _resetRemoteDedup(_remotes[i], bootId);
            if (sessionChanged) *sessionChanged = true;
        } else if (bootId != 0 && _remotes[i].bootId == 0) {
            _resetRemoteDedup(_remotes[i], bootId);
            if (sessionChanged) *sessionChanged = true;
        }

        if (touch) _markRemoteSeen(srcId, ip, port);
        (void)seq;
        return true;
    }

    for (int i = 0; i < IOT_MAX_REMOTES; i++) {
        if (!_remotes[i].active) {
            memset(&_remotes[i], 0, sizeof(RemoteDevice));
            _remotes[i].id = srcId;
            _remotes[i].bootId = bootId;
            _remotes[i].state = DeviceState::UNKNOWN;
            _remotes[i].active = true;
            if (sessionChanged && bootId != 0) *sessionChanged = true;
            if (touch) _markRemoteSeen(srcId, ip, port);
            return true;
        }
    }

    int oldest = 0;
    for (int i = 1; i < IOT_MAX_REMOTES; i++) {
        if (_remotes[i].lastSeen < _remotes[oldest].lastSeen) oldest = i;
    }
    memset(&_remotes[oldest], 0, sizeof(RemoteDevice));
    _remotes[oldest].id = srcId;
    _remotes[oldest].bootId = bootId;
    _remotes[oldest].state = DeviceState::UNKNOWN;
    _remotes[oldest].active = true;
    if (sessionChanged && bootId != 0) *sessionChanged = true;
    if (touch) _markRemoteSeen(srcId, ip, port);
    return true;
}

void IoTNode::_fillRemoteFromHello(RemoteDevice &dev, const IoTPacket &pkt) {
    uint8_t devType = 0;
    if (pkt.getTLV_uint8(TlvTag::DEVICE_TYPE_TAG, devType)) {
        dev.deviceType = devType;
    }
    pkt.getTLV_string(TlvTag::DEVICE_NAME, dev.name, sizeof(dev.name));
    pkt.getTLV_string(TlvTag::FW_VERSION, dev.fwVersion, sizeof(dev.fwVersion));
}

void IoTNode::registerRemote(uint8_t id, IPAddress ip, uint16_t port) {
    _updateRemote(id, ip, port, 0, 0);
}

RemoteDevice* IoTNode::getRemote(uint8_t id) {
    for (int i = 0; i < IOT_MAX_REMOTES; i++) {
        if (_remotes[i].active && _remotes[i].id == id) return &_remotes[i];
    }
    return nullptr;
}

uint8_t IoTNode::getRemoteCount() const {
    uint8_t count = 0;
    for (int i = 0; i < IOT_MAX_REMOTES; i++) {
        if (_remotes[i].active) count++;
    }
    return count;
}

// ============================================================
// ONLINE / STALE / OFFLINE state machine (Fix V4.1.1)
// ============================================================

void IoTNode::_updateDeviceStates() {
    unsigned long ahora = millis();
    for (int i = 0; i < IOT_MAX_REMOTES; i++) {
        if (!_remotes[i].active) continue;
        unsigned long elapsed = ahora - _remotes[i].lastSeen;

        DeviceState newState;
        if (elapsed < IOT_STALE_TIMEOUT_MS) {
            newState = DeviceState::ONLINE;
        } else if (elapsed < IOT_OFFLINE_TIMEOUT_MS) {
            newState = DeviceState::STALE;
        } else {
            newState = DeviceState::OFFLINE;
        }
        _remotes[i].state = newState;
    }
}

void IoTNode::updateDeviceStates() {
    _updateDeviceStates();
}

// ============================================================
// Heartbeat
// ============================================================

void IoTNode::enableHeartbeat(IPAddress destIP, uint16_t destPort, unsigned long intervalMs) {
    _hbEnabled = true;
    _hbIP = destIP;
    _hbPort = destPort;
    _hbInterval = intervalMs;
    _lastHb = millis();
}

void IoTNode::disableHeartbeat() { _hbEnabled = false; }

void IoTNode::_sendHeartbeat() {
    unsigned long ahora = millis();
    if (ahora - _lastHb < _hbInterval) return;

    // Jitter: ±5 seconds randomizado para distribuir tráfico
    // cuando hay muchos dispositivos en la red
    long jitter = random(-5000, 5000);
    _lastHb = ahora + jitter;

    IoTPacket pkt;
    pkt.version = IOT_PROTOCOL_VER;
    pkt.type = MsgType::HEARTBEAT;
    pkt.src = _deviceId;
    pkt.dst = IOT_DEVICE_CENTRAL;
    pkt.bootId = _bootId;
    pkt.seq = getNextSeq();
    pkt.flags = IOT_FLAG_BACKGROUND;
    pkt.clearPayload();

    // Telemetría enriquecida
    addTlvChecked(pkt.addTLV_uint32(TlvTag::UPTIME_SEC, millis() / 1000),
                  "HEARTBEAT", "UPTIME_SEC");
    addTlvChecked(pkt.addTLV_int8(TlvTag::RSSI_VAL, (int8_t)WiFi.RSSI()),
                  "HEARTBEAT", "RSSI_VAL");
    addTlvChecked(pkt.addTLV_uint32(TlvTag::FREE_HEAP, ESP.getFreeHeap()),
                  "HEARTBEAT", "FREE_HEAP");
    addTlvChecked(pkt.addTLV_uint8(TlvTag::QUEUE_DEPTH, _queueCount),
                  "HEARTBEAT", "QUEUE_DEPTH");
    addTlvChecked(pkt.addTLV_uint32(TlvTag::TX_COUNT, _stats.txPackets),
                  "HEARTBEAT", "TX_COUNT");
    addTlvChecked(pkt.addTLV_uint32(TlvTag::ACK_TIMEOUTS, _stats.ackTimeouts),
                  "HEARTBEAT", "ACK_TIMEOUTS");
    if (_firmwareVersion && _firmwareVersion[0] != '\0') {
        addTlvChecked(pkt.addTLV_string(TlvTag::FW_VERSION, _firmwareVersion),
                      "HEARTBEAT", "FW_VERSION");
    }
    addTlvChecked(pkt.addTLV_uint8(TlvTag::BOOT_REASON,
                                   static_cast<uint8_t>(_bootReason)),
                  "HEARTBEAT", "BOOT_REASON");

    sendDirect(pkt, _hbIP, _hbPort);
}

// ============================================================
// Discovery (HELLO)
// ============================================================

void IoTNode::sendHello(IPAddress destIP, uint16_t destPort,
                        uint8_t deviceType, const char* devName) {
    IoTPacket pkt;
    pkt.version = IOT_PROTOCOL_VER;
    pkt.type = MsgType::HELLO;
    pkt.src = _deviceId;
    pkt.dst = IOT_DEVICE_CENTRAL;
    pkt.bootId = _bootId;
    pkt.seq = getNextSeq();
    pkt.flags = IOT_FLAG_ACK_REQUIRED | IOT_FLAG_RELIABLE;
    pkt.clearPayload();

    // HELLO policy: DEVICE_TYPE_TAG, DEVICE_NAME and BOOT_ID_TAG are the
    // identity fields required for a useful discovery record. If one does
    // not fit, do not enqueue a misleading partial HELLO. FW_VERSION is
    // optional metadata: its absence is logged, but discovery can proceed.
    const bool hasDeviceType = pkt.addTLV_uint8(TlvTag::DEVICE_TYPE_TAG, deviceType);
    const bool hasDeviceName = pkt.addTLV_string(TlvTag::DEVICE_NAME, devName);
    const bool hasFwVersion =
        _firmwareVersion && _firmwareVersion[0] != '\0' &&
        pkt.addTLV_string(TlvTag::FW_VERSION, _firmwareVersion);
    const bool hasBootReason = pkt.addTLV_uint8(
        TlvTag::BOOT_REASON, static_cast<uint8_t>(_bootReason));
    const bool hasBootId = pkt.addTLV_uint16(TlvTag::BOOT_ID_TAG, _bootId);
    uint8_t missingCritical = 0;
    missingCritical += !hasDeviceType;
    missingCritical += !hasDeviceName;
    missingCritical += !hasBootId;
    if (!hasFwVersion) {
        Serial.printf("[W] HELLO: TLV FW_VERSION opcional ausente\n");
    }
    if (!hasBootReason) {
        Serial.printf("[W] HELLO: TLV BOOT_REASON opcional descartado por falta de payload\n");
    }
    if (missingCritical != 0) {
        Serial.printf("[W] HELLO: %u TLV(s) críticos faltantes\n", missingCritical);
    }
    if (!hasDeviceType || !hasDeviceName || !hasBootId) {
        // A discovery packet without identity/session data cannot be safely
        // registered. Unlike EVENT, HELLO may tolerate only missing optional
        // metadata, not missing critical identity fields.
        Serial.printf("[W] HELLO descartado: falta un TLV crítico\n");
        return;
    }

    const bool queued = enqueue(pkt, destIP, destPort);
    if (!queued) {
        Serial.printf("[W] HELLO no encolado: cola llena o proveedor de auth fallo\n");
    } else if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("[I] HELLO encolado sin WiFi: se enviara al reconectar (q=%u)\n",
                      static_cast<unsigned>(queuedCount()));
    }
}

void IoTNode::setFirmwareVersion(const char* version) {
    _firmwareVersion = version;
}

void IoTNode::setBootReason(BootReason reason) {
    _bootReason = reason;
}

void IoTNode::onPacketReceived(IoTPacketHandler handler) {
    _handler = handler;
}

void IoTNode::setAuthProvider(const IoTAuthProvider &provider) {
    _authProvider = provider;
    if (_authProvider.mode == IoTAuthMode::DISABLED) {
        // DISABLED es un bypass total aunque el caller deje callbacks.
        _authProvider.signOutgoing = false;
    }
}

void IoTNode::clearAuthProvider() {
    memset(&_authProvider, 0, sizeof(_authProvider));
    _authProvider.mode = IoTAuthMode::DISABLED;
}

bool IoTNode::_verifyIncoming(const IoTPacket &pkt, IPAddress remoteIP,
                              uint16_t remotePort) {
    if (_authProvider.mode == IoTAuthMode::DISABLED) return true;

    const bool markedAuthenticated = (pkt.flags & IOT_FLAG_AUTHENTICATED) != 0;
    bool accepted = false;
    if (!markedAuthenticated && _authProvider.mode == IoTAuthMode::OPTIONAL) {
        accepted = true;
    } else if (_authProvider.verify) {
        accepted = _authProvider.verify(pkt, _authProvider.context);
    }

    if (!accepted && _authProvider.onRejected) {
        // Callback diagnóstico: se invoca después del parse y antes de todos
        // los efectos internos de recepción.
        _authProvider.onRejected(pkt, remoteIP, remotePort);
    }
    return accepted;
}

bool IoTNode::_prepareOutgoing(IoTPacket &pkt) {
    if (_authProvider.mode == IoTAuthMode::DISABLED) return true;

    // REQUIRED implica firma saliente aunque signOutgoing no se haya marcado.
    const bool mustSign = _authProvider.signOutgoing ||
                          _authProvider.mode == IoTAuthMode::REQUIRED;
    if (!mustSign) return true;
    if (!_authProvider.sign) return false;
    return _authProvider.sign(pkt, _authProvider.context);
}

bool IoTNode::sendEvent(uint8_t eventCode, IPAddress destIP, uint16_t destPort, uint8_t destId) {
    IoTPacket pkt;
    pkt.version = IOT_PROTOCOL_VER;
    pkt.type = MsgType::EVENT;
    pkt.src = _deviceId;
    pkt.dst = destId;
    pkt.bootId = _bootId;
    pkt.seq = getNextSeq();
    pkt.flags = IOT_FLAG_ACK_REQUIRED | IOT_FLAG_RELIABLE;
    pkt.clearPayload();

    // EVENT policy: EVENT_TYPE, EVENT_VALUE and RSSI are built as one
    // reliable application record. A partial event could trigger an action
    // without complete telemetry, so any missing TLV aborts the enqueue.
    uint8_t missing = 0;
    missing += !addTlvChecked(pkt.addTLV_uint8(TlvTag::EVENT_TYPE, eventCode),
                              "EVENT", "EVENT_TYPE");
    missing += !addTlvChecked(pkt.addTLV_uint8(TlvTag::EVENT_VALUE, 1),
                              "EVENT", "EVENT_VALUE");
    missing += !addTlvChecked(pkt.addTLV_int8(TlvTag::RSSI_VAL, (int8_t)WiFi.RSSI()),
                              "EVENT", "RSSI_VAL");
    if (missing != 0) return false;

    return enqueue(pkt, destIP, destPort);
}


// ============================================================
// Estadísticas
// ============================================================

void IoTNode::resetStats() {
    memset(&_stats, 0, sizeof(_stats));
    memset(&_rtt, 0, sizeof(_rtt));
    _rtt.minMs = 0xFFFFFFFF;
}
