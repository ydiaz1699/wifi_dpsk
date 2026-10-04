/**
 * IoTConfigHandler V4.3 — Procesador de mensajes CONFIG remotos
 *
 * Maneja mensajes CONFIG enviados por la central para cambiar
 * parámetros del dispositivo en runtime (sin recompilar).
 *
 * Parámetros configurables:
 * - Heartbeat interval
 * - Antirebote sensor
 * - Device name
 * - Auth enable/disable
 * - Reboot command
 * - Reset stats
 *
 * Flujo:
 *   Central → CONFIG (con TLVs de parámetros) → Nodo
 *   Nodo → aplica cambios → persiste en LittleFS → RESPONSE OK
 *
 * Uso en el emisor:
 *   IoTConfigHandler configHandler(storage, node);
 *   // En onPacketReceived:
 *   case MsgType::CONFIG:
 *       configHandler.handleConfig(pkt, remoteIP, remotePort);
 *       break;
 */

#pragma once
#include "IoTProtocol.h"
#include "IoTStorage.h"
#include "IoTNode.h"

// Callback opcional: se llama después de aplicar una config exitosa.
// Permite al usuario reaccionar (ej: reiniciar heartbeat timer).
typedef void (*ConfigAppliedCallback)(const IoTConfig &newConfig);

class IoTConfigHandler {
public:
    /**
     * Constructor.
     * @param storage  Referencia al IoTStorage del dispositivo
     * @param node     Referencia al IoTNode (para enviar RESPONSE y resetear stats)
     */
    IoTConfigHandler(IoTStorage &storage, IoTNode &node);

    /**
     * Procesar un paquete CONFIG recibido.
     * Extrae los TLV, aplica los cambios, persiste y envía RESPONSE.
     * @return true si al menos un parámetro fue cambiado
     */
    bool handleConfig(const IoTPacket &pkt, IPAddress remoteIP, uint16_t remotePort);

    /**
     * Registrar callback para cuando se aplica una config.
     */
    void onConfigApplied(ConfigAppliedCallback cb) { _callback = cb; }

private:
    IoTStorage &_storage;
    IoTNode &_node;
    ConfigAppliedCallback _callback;

    void _sendResponse(const IoTPacket &originalPkt, IPAddress destIP, uint16_t destPort,
                       ResultCode result, uint8_t changesApplied);
};
