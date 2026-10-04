/**
 * IoTProtocol V4.1 — Implementación
 *
 * Cambios vs V4.0:
 * - SEQ 32 bits (big-endian en wire)
 * - BOOT_ID 16 bits en cabecera
 * - PAY_LEN ahora 1 byte (max 64, suficiente)
 * - Validación estricta: len == expected, version major check
 * - TLV: tamaño exacto para tipos fijos
 * - iot_validate_tlv() para validar payload completo
 */

#include "IoTProtocol.h"
#include <string.h>

// ============================================================
// CRC16-CCITT (polinomio 0x1021, init 0xFFFF)
// ============================================================

uint16_t iot_crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x8000)
                crc = (crc << 1) ^ 0x1021;
            else
                crc <<= 1;
        }
    }
    return crc;
}

// ============================================================
// TLV — Escribir
// ============================================================

void IoTPacket::clearPayload() {
    payloadLen = 0;
    memset(payload, 0, IOT_MAX_PAYLOAD);
}

static bool addTLV_raw(uint8_t* payload, uint8_t &payloadLen, TlvTag tag, const uint8_t* data, uint8_t dataLen) {
    if ((uint16_t)payloadLen + 2 + dataLen > IOT_MAX_PAYLOAD) return false;
    payload[payloadLen++] = static_cast<uint8_t>(tag);
    payload[payloadLen++] = dataLen;
    memcpy(&payload[payloadLen], data, dataLen);
    payloadLen += dataLen;
    return true;
}

bool IoTPacket::addTLV_uint8(TlvTag tag, uint8_t value) {
    return addTLV_raw(payload, payloadLen, tag, &value, 1);
}

bool IoTPacket::addTLV_int8(TlvTag tag, int8_t value) {
    return addTLV_raw(payload, payloadLen, tag, (uint8_t*)&value, 1);
}

bool IoTPacket::addTLV_uint16(TlvTag tag, uint16_t value) {
    uint8_t buf[2];
    buf[0] = (value >> 8) & 0xFF;  // Big-endian
    buf[1] = value & 0xFF;
    return addTLV_raw(payload, payloadLen, tag, buf, 2);
}

bool IoTPacket::addTLV_int16(TlvTag tag, int16_t value) {
    return addTLV_uint16(tag, (uint16_t)value);
}

bool IoTPacket::addTLV_uint32(TlvTag tag, uint32_t value) {
    uint8_t buf[4];
    buf[0] = (value >> 24) & 0xFF;
    buf[1] = (value >> 16) & 0xFF;
    buf[2] = (value >> 8) & 0xFF;
    buf[3] = value & 0xFF;
    return addTLV_raw(payload, payloadLen, tag, buf, 4);
}

bool IoTPacket::addTLV_string(TlvTag tag, const char* str) {
    uint8_t len = (uint8_t)strlen(str);
    if ((uint16_t)payloadLen + 2 + len > IOT_MAX_PAYLOAD) return false;
    return addTLV_raw(payload, payloadLen, tag, (const uint8_t*)str, len);
}

// ============================================================
// TLV — Leer (validación ESTRICTA de tamaño)
// ============================================================

static bool findTLV(const uint8_t* payload, uint8_t payloadLen, TlvTag tag,
                    const uint8_t* &valuePtr, uint8_t &valueLen) {
    uint8_t offset = 0;
    while (offset + 2 <= payloadLen) {
        uint8_t t = payload[offset];
        uint8_t l = payload[offset + 1];
        if ((uint16_t)offset + 2 + l > payloadLen) return false;  // Corrupto
        if (t == static_cast<uint8_t>(tag)) {
            valuePtr = &payload[offset + 2];
            valueLen = l;
            return true;
        }
        offset += 2 + l;
    }
    return false;
}

bool IoTPacket::hasTLV(TlvTag tag) const {
    const uint8_t* ptr; uint8_t len;
    return findTLV(payload, payloadLen, tag, ptr, len);
}

// ESTRICTO: uint8 requiere exactamente 1 byte
bool IoTPacket::getTLV_uint8(TlvTag tag, uint8_t &value) const {
    const uint8_t* ptr; uint8_t len;
    if (!findTLV(payload, payloadLen, tag, ptr, len)) return false;
    if (len != 1) return false;  // ESTRICTO
    value = ptr[0];
    return true;
}

bool IoTPacket::getTLV_int8(TlvTag tag, int8_t &value) const {
    const uint8_t* ptr; uint8_t len;
    if (!findTLV(payload, payloadLen, tag, ptr, len)) return false;
    if (len != 1) return false;  // ESTRICTO
    value = (int8_t)ptr[0];
    return true;
}

// ESTRICTO: uint16 requiere exactamente 2 bytes
bool IoTPacket::getTLV_uint16(TlvTag tag, uint16_t &value) const {
    const uint8_t* ptr; uint8_t len;
    if (!findTLV(payload, payloadLen, tag, ptr, len)) return false;
    if (len != 2) return false;  // ESTRICTO
    value = ((uint16_t)ptr[0] << 8) | ptr[1];
    return true;
}

bool IoTPacket::getTLV_int16(TlvTag tag, int16_t &value) const {
    uint16_t raw;
    if (!getTLV_uint16(tag, raw)) return false;
    value = (int16_t)raw;
    return true;
}

// ESTRICTO: uint32 requiere exactamente 4 bytes
bool IoTPacket::getTLV_uint32(TlvTag tag, uint32_t &value) const {
    const uint8_t* ptr; uint8_t len;
    if (!findTLV(payload, payloadLen, tag, ptr, len)) return false;
    if (len != 4) return false;  // ESTRICTO
    value = ((uint32_t)ptr[0] << 24) | ((uint32_t)ptr[1] << 16) |
            ((uint32_t)ptr[2] << 8) | ptr[3];
    return true;
}

// String: cualquier longitud > 0 es válida
bool IoTPacket::getTLV_string(TlvTag tag, char* buf, uint8_t maxLen) const {
    const uint8_t* ptr; uint8_t len;
    if (!findTLV(payload, payloadLen, tag, ptr, len)) return false;
    if (len == 0) return false;
    uint8_t copyLen = (len < maxLen - 1) ? len : (maxLen - 1);
    memcpy(buf, ptr, copyLen);
    buf[copyLen] = '\0';
    return true;
}

// ============================================================
// Validación estructural del payload TLV
// ============================================================

bool iot_validate_tlv(const uint8_t* payload, uint8_t payloadLen) {
    uint8_t offset = 0;
    while (offset < payloadLen) {
        if (offset + 2 > payloadLen) return false;  // No cabe TAG+LEN
        uint8_t l = payload[offset + 1];
        if ((uint16_t)offset + 2 + l > payloadLen) return false;  // Desbordamiento
        offset += 2 + l;
    }
    return (offset == payloadLen);  // Debe consumir exactamente todo
}

// ============================================================
// Serialización: IoTPacket → wire format
// ============================================================

size_t iot_serialize(const IoTPacket &pkt, uint8_t* buf, size_t bufSize) {
    size_t total = IOT_HEADER_SIZE + pkt.payloadLen + IOT_CRC_SIZE;
    if (total > bufSize) return 0;
    if (pkt.payloadLen > IOT_MAX_PAYLOAD) return 0;

    size_t i = 0;

    // Magic
    buf[i++] = IOT_MAGIC_0;
    buf[i++] = IOT_MAGIC_1;

    // Version
    buf[i++] = pkt.version;

    // Type
    buf[i++] = static_cast<uint8_t>(pkt.type);

    // Source
    buf[i++] = pkt.src;

    // Destination
    buf[i++] = pkt.dst;

    // Boot ID (big-endian)
    buf[i++] = (pkt.bootId >> 8) & 0xFF;
    buf[i++] = pkt.bootId & 0xFF;

    // Sequence (32-bit big-endian)
    buf[i++] = (pkt.seq >> 24) & 0xFF;
    buf[i++] = (pkt.seq >> 16) & 0xFF;
    buf[i++] = (pkt.seq >> 8) & 0xFF;
    buf[i++] = pkt.seq & 0xFF;

    // Flags
    buf[i++] = pkt.flags;

    // Payload length (1 byte, max 64)
    buf[i++] = pkt.payloadLen;

    // Payload
    memcpy(&buf[i], pkt.payload, pkt.payloadLen);
    i += pkt.payloadLen;

    // CRC16 (sobre todo lo anterior)
    uint16_t crc = iot_crc16(buf, i);
    buf[i++] = (crc >> 8) & 0xFF;
    buf[i++] = crc & 0xFF;

    return i;
}

// ============================================================
// Deserialización: wire format → IoTPacket
// ============================================================

bool iot_deserialize(const uint8_t* buf, size_t len, IoTPacket &pkt) {
    // Mínimo: header + CRC (sin payload)
    if (len < IOT_HEADER_SIZE + IOT_CRC_SIZE) return false;

    // Verificar magic
    if (buf[0] != IOT_MAGIC_0 || buf[1] != IOT_MAGIC_1) return false;

    // Verificar versión (major debe coincidir)
    uint8_t ver = buf[2];
    if ((ver >> 4) != IOT_PROTOCOL_MAJOR) return false;

    // Leer payload length
    uint8_t payLen = buf[13];
    if (payLen > IOT_MAX_PAYLOAD) return false;

    // Verificar longitud EXACTA
    size_t expectedLen = IOT_HEADER_SIZE + payLen + IOT_CRC_SIZE;
    if (len != expectedLen) return false;  // ESTRICTO: debe ser exacto

    // Verificar CRC
    uint16_t crcRecibido = ((uint16_t)buf[expectedLen - 2] << 8) | buf[expectedLen - 1];
    uint16_t crcCalculado = iot_crc16(buf, expectedLen - 2);
    if (crcRecibido != crcCalculado) return false;

    // Parsear cabecera
    pkt.version   = ver;
    pkt.type      = static_cast<MsgType>(buf[3]);
    pkt.src       = buf[4];
    pkt.dst       = buf[5];
    pkt.bootId    = ((uint16_t)buf[6] << 8) | buf[7];
    pkt.seq       = ((uint32_t)buf[8] << 24) | ((uint32_t)buf[9] << 16) |
                    ((uint32_t)buf[10] << 8) | buf[11];
    pkt.flags     = buf[12];
    pkt.payloadLen = payLen;

    // Copiar payload
    if (payLen > 0) {
        memcpy(pkt.payload, &buf[IOT_HEADER_SIZE], payLen);
    }

    // Validar estructura TLV del payload
    if (payLen > 0 && !iot_validate_tlv(pkt.payload, payLen)) {
        return false;  // Payload corrupto estructuralmente
    }

    return true;
}
