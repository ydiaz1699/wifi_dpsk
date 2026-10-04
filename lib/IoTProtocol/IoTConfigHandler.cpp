/**
 * IoTConfigHandler V4.3 — Implementación
 */

#include "IoTConfigHandler.h"
#include <string.h>

// ============================================================
// Constructor
// ============================================================

IoTConfigHandler::IoTConfigHandler(IoTStorage &storage, IoTNode &node)
    : _storage(storage), _node(node), _callback(nullptr)
{}

// ============================================================
// Procesar CONFIG
// ============================================================

bool IoTConfigHandler::handleConfig(const IoTPacket &pkt, IPAddress remoteIP, uint16_t remotePort) {
    IoTConfig &cfg = _storage.config();
    uint8_t changesApplied = 0;

    // --- Heartbeat interval ---
    uint32_t newHb = 0;
    if (pkt.getTLV_uint32(TlvTag::CFG_HEARTBEAT_MS, newHb)) {
        if (newHb >= 5000 && newHb <= 600000) {  // 5s – 10min
            cfg.heartbeatIntervalMs = newHb;
            changesApplied++;
        }
    }

    // --- Antirebote ---
    uint32_t newAntirebote = 0;
    if (pkt.getTLV_uint32(TlvTag::CFG_ANTIREBOTE_MS, newAntirebote)) {
        if (newAntirebote >= 100 && newAntirebote <= 10000) {  // 100ms – 10s
            cfg.antireboteMs = newAntirebote;
            changesApplied++;
        }
    }

    // --- Device name ---
    char newName[IOT_STORAGE_NAME_MAX] = "";
    if (pkt.getTLV_string(TlvTag::CFG_DEVICE_NAME, newName, sizeof(newName))) {
        if (strlen(newName) > 0) {
            strncpy(cfg.deviceName, newName, IOT_STORAGE_NAME_MAX - 1);
            cfg.deviceName[IOT_STORAGE_NAME_MAX - 1] = '\0';
            changesApplied++;
        }
    }

    // --- Auth enable/disable ---
    uint8_t authEnable = 0xFF;
    if (pkt.getTLV_uint8(TlvTag::CFG_AUTH_ENABLE, authEnable)) {
        if (authEnable <= 1) {
            cfg.authEnabled = (authEnable == 1);
            changesApplied++;
        }
    }

    // --- Reset stats ---
    uint8_t resetStats = 0;
    if (pkt.getTLV_uint8(TlvTag::CFG_RESET_STATS, resetStats)) {
        if (resetStats == 1) {
            _node.resetStats();
            changesApplied++;
        }
    }

    // --- Config version (tracking) ---
    uint8_t cfgVer = 0;
    if (pkt.getTLV_uint8(TlvTag::CFG_VERSION, cfgVer)) {
        cfg.configVersion = cfgVer;
    }

    // --- Persistir si hubo cambios ---
    ResultCode result = ResultCode::OK;
    if (changesApplied > 0) {
        if (!_storage.saveConfig()) {
            result = ResultCode::FAIL;
        }
    }

    // --- Enviar RESPONSE ---
    _sendResponse(pkt, remoteIP, remotePort, result, changesApplied);

    // --- Callback ---
    if (changesApplied > 0 && _callback) {
        _callback(cfg);
    }

    // --- Reboot (al final, después de todo) ---
    uint8_t reboot = 0;
    if (pkt.getTLV_uint8(TlvTag::CFG_REBOOT, reboot)) {
        if (reboot == 1) {
            delay(100);  // Dar tiempo a que el RESPONSE se envíe
            ESP.restart();
        }
    }

    return (changesApplied > 0);
}

// ============================================================
// Enviar RESPONSE
// ============================================================

void IoTConfigHandler::_sendResponse(const IoTPacket &originalPkt, IPAddress destIP,
                                     uint16_t destPort, ResultCode result,
                                     uint8_t changesApplied) {
    IoTPacket resp;
    resp.version = IOT_PROTOCOL_VER;
    resp.type = MsgType::RESPONSE;
    resp.src = _node.getDeviceId();
    resp.dst = originalPkt.src;
    resp.bootId = _node.getBootId();
    resp.seq = _node.getNextSeq();
    resp.flags = 0;
    resp.clearPayload();

    if (!resp.addTLV_uint8(TlvTag::RESULT_CODE, static_cast<uint8_t>(result))) {
        Serial.printf("[W] CONFIG RESPONSE: TLV RESULT_CODE descartado por falta de payload\n");
        return;
    }
    if (!resp.addTLV_uint8(TlvTag::CFG_VERSION, _storage.config().configVersion)) {
        Serial.printf("[W] CONFIG RESPONSE: TLV CFG_VERSION descartado por falta de payload\n");
        return;
    }

    // Si hay CMD_ID en el original, incluirlo en la respuesta
    uint32_t cmdId = 0;
    if (originalPkt.getTLV_uint32(TlvTag::CMD_ID, cmdId) &&
        !resp.addTLV_uint32(TlvTag::RESULT_CMD_ID, cmdId)) {
        Serial.printf("[W] CONFIG RESPONSE: TLV RESULT_CMD_ID descartado por falta de payload\n");
        return;
    }

    _node.sendDirect(resp, destIP, destPort);
}
