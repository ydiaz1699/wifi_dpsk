/**
 * IoTProtocol V4.3 — Protocolo binario universal para redes ESP8266/ESP32
 *
 * El paquete actual es V4.3; el formato wire permanece compatible con V4.1
 * (`IOT_PROTOCOL_VER = 0x41`). Este header define el contrato wire común;
 * las mejoras de storage, HMAC y operación viven en sus módulos respectivos.
 *
 * Contrato wire introducido en V4.1:
 * - SEQ ampliado de 16 a 32 bits (elimina preocupaciones por rollover)
 * - BOOT_ID de 16 bits en cabecera (resuelve reinicio + SEQ vuelve a 1)
 * - Validación estricta: len == expectedLen, version check
 * - TLV estricto: tamaño exacto para tipos fijos
 * - Separación clara: ACK = recibí, RESPONSE = ejecuté
 * - Códigos de error definidos
 * - Flags: PRIORITY_HIGH, PRIORITY_BG para cola con prioridades
 *
 * Formato del paquete V4.1:
 * ┌───────┬─────┬──────┬─────┬─────┬────────┬──────┬───────┬─────────┬─────────┬───────┐
 * │ MAGIC │ VER │ TYPE │ SRC │ DST │BOOT_ID │ SEQ  │ FLAGS │ PAY_LEN │ PAYLOAD │ CRC16 │
 * │ 2B    │ 1B  │ 1B   │ 1B  │ 1B  │ 2B     │ 4B   │ 1B    │ 1B      │ N B     │ 2B    │
 * └───────┴─────┴──────┴─────┴─────┴────────┴──────┴───────┴─────────┴─────────┴───────┘
 *
 * Header total: 14 bytes + payload + 2 bytes CRC = 16 + N bytes mínimo
 *
 * TLV format (dentro de payload):
 * ┌──────┬────────┬───────┐
 * │ TAG  │ LENGTH │ VALUE │
 * │ 1B   │ 1B     │ N B   │
 * └──────┴────────┴───────┘
 */

#pragma once
#include <Arduino.h>

// ============================================================
// Versión del protocolo
// ============================================================

#define IOT_PROTOCOL_MAJOR  4
#define IOT_PROTOCOL_MINOR  1
#define IOT_PROTOCOL_VER    0x41   // Wire format: major<<4 | minor (compacto)

// ============================================================
// Constantes del protocolo
// ============================================================

#define IOT_MAGIC_0       0xA5
#define IOT_MAGIC_1       0x5A

// Header: MAGIC(2) + VER(1) + TYPE(1) + SRC(1) + DST(1) + BOOT_ID(2) + SEQ(4) + FLAGS(1) + PAY_LEN(1)
#define IOT_HEADER_SIZE   14
#define IOT_CRC_SIZE      2
#define IOT_MAX_PAYLOAD   64
#define IOT_MAX_PACKET    (IOT_HEADER_SIZE + IOT_MAX_PAYLOAD + IOT_CRC_SIZE)  // 80 bytes max

// Identidad legible: 23 bytes útiles + terminador NUL.
#define IOT_DEVICE_NAME_MAX 24

// Alias histórico para consumidores que aún incluyen el nombre anterior.
#define IOT_STORAGE_NAME_MAX IOT_DEVICE_NAME_MAX

// ============================================================
// Device IDs (1 byte: 0-255)
// ============================================================
// Convención (no restricción en código):
// 0x00 = reservado (no usar)
// 0x01 = nodo coordinador
// 0x02–0x1F = nodos de entrada
// 0x20–0x3F = nodos de interacción
// 0x40–0x5F = nodos de telemetría
// 0x60–0x7F = nodos de salida
// 0x80–0x9F = nodos de visualización
// 0xA0–0xFE = futuros
// 0xFF = BROADCAST

#define IOT_DEVICE_CENTRAL    0x01
#define IOT_DEVICE_BROADCAST  0xFF

// ============================================================
// Tipos de mensaje (1 byte)
// ============================================================

enum class MsgType : uint8_t {
    // Aplicación
    EVENT       = 0x01,   // Nodo → destino: evento discreto
    DATA        = 0x02,   // Nodo → destino: datos continuos
    COMMAND     = 0x03,   // Controlador → nodo de salida: orden
    RESPONSE    = 0x04,   // Nodo de salida → controlador: resultado
    DISPLAY_MSG = 0x05,   // Controlador → nodo de visualización: datos

    // Control
    ACK         = 0x10,   // Confirmación de recepción (protocolo, NO ejecución)
    HEARTBEAT   = 0x11,   // Estoy vivo + telemetría básica
    STATUS      = 0x12,   // Estado completo del dispositivo
    STATE_REPORT  = 0x13, // Nodo → Central: estado actual de sensores/actuadores
    STATE_REQUEST = 0x14, // Central → Nodo(s): pedido de re-publicar estado

    // Discovery
    HELLO       = 0x20,   // Dispositivo anuncia presencia al boot
    HELLO_ACK   = 0x21,   // Central confirma registro

    // Configuración
    CONFIG      = 0x30,   // Central → Dispositivo: parámetros de configuración

    // Error
    ERROR_MSG   = 0xE0,   // Error genérico con código
};

// ============================================================
// Flags (1 byte, bitmap)
// ============================================================

#define IOT_FLAG_ACK_REQUIRED   0x01   // Emisor espera ACK
#define IOT_FLAG_RELIABLE       0x02   // Reintentar con backoff si no hay ACK
#define IOT_FLAG_URGENT         0x04   // Prioridad URGENT (procesar antes en cola)
#define IOT_FLAG_BACKGROUND     0x08   // Prioridad baja (descartable si cola llena)
#define IOT_FLAG_AUTHENTICATED  0x10   // Payload incluye autenticación del proveedor
// Si ni URGENT ni BACKGROUND: prioridad NORMAL

// ============================================================
// Prioridades derivadas de flags
// ============================================================

enum class Priority : uint8_t {
    URGENT      = 0,   // Mensaje crítico, no descartable por prioridad
    NORMAL      = 1,   // Mensaje estándar
    BACKGROUND  = 2,   // Telemetría, descartable si la cola está llena
};

// ============================================================
// TLV Tags (1 byte)
// ============================================================

enum class TlvTag : uint8_t {
    // === Eventos (0x01–0x0F) ===
    EVENT_TYPE      = 0x01,   // uint8_t: identificador de evento de aplicación
    EVENT_VALUE     = 0x02,   // uint8_t: valor definido por la aplicación

    // === Datos de sensores (0x10–0x2F) ===
    TEMPERATURE     = 0x10,   // int16_t: temp × 10 (237 = 23.7°C)
    HUMIDITY        = 0x11,   // uint16_t: hum × 10 (482 = 48.2%)
    PRESSURE        = 0x12,   // uint16_t: hPa
    LIGHT           = 0x13,   // uint16_t: lux
    BATTERY_PCT     = 0x14,   // uint8_t: 0–100%
    BATTERY_MV      = 0x15,   // uint16_t: mV
    RSSI_VAL        = 0x16,   // int8_t: dBm

    // === Comandos (0x30–0x3F) ===
    CMD_STATE       = 0x30,   // uint8_t: 0=OFF, 1=ON, 2=TOGGLE
    CMD_DURATION    = 0x31,   // uint16_t: ms
    CMD_CHANNEL     = 0x32,   // uint8_t: canal (0–7)
    CMD_ID          = 0x33,   // uint32_t: ID lógico del comando (para RESPONSE matching)

    // === Respuesta (0x40–0x4F) ===
    RESULT_CODE     = 0x40,   // uint8_t: ResultCode enum
    RESULT_STATE    = 0x41,   // uint8_t: estado actual tras ejecutar
    RESULT_CMD_ID   = 0x42,   // uint32_t: CMD_ID al que responde

    // === Display (0x50–0x5F) ===
    DISPLAY_LINE    = 0x50,   // uint8_t: línea (0–3)
    DISPLAY_TEXT    = 0x51,   // string: texto
    DISPLAY_CLEAR   = 0x52,   // uint8_t: 1=clear

    // === Discovery/Config (0x60–0x7F) ===
    DEVICE_NAME     = 0x60,   // string: nombre legible
    DEVICE_TYPE_TAG = 0x61,   // uint8_t: identificador de tipo de aplicación
    CAPABILITY      = 0x62,   // uint8_t: capacidad (puede repetirse)
    FW_VERSION      = 0x63,   // string: "4.1.0"
    BOOT_ID_TAG     = 0x64,   // uint16_t: boot_id (para HELLO)

    // Config remota (Central → Nodo via MsgType::CONFIG)
    CFG_HEARTBEAT_MS  = 0x70, // uint32_t: nuevo intervalo heartbeat (ms)
    CFG_ANTIREBOTE_MS = 0x71, // uint32_t: nuevo antirebote (ms)
    CFG_DEVICE_NAME   = 0x72, // string: nuevo nombre de dispositivo
    CFG_AUTH_ENABLE   = 0x73, // uint8_t: 0=disable, 1=enable auth
    CFG_REBOOT        = 0x74, // uint8_t: 1=reiniciar dispositivo
    CFG_RESET_STATS   = 0x75, // uint8_t: 1=resetear estadísticas
    CFG_VERSION       = 0x76, // uint8_t: versión de la config aplicada

    // === Timestamps/Telemetría (0x80–0x8F) ===
    UPTIME_SEC      = 0x80,   // uint32_t: segundos desde boot
    TIMESTAMP_MS    = 0x81,   // uint32_t: millis() del emisor

    // === Status/Diag (0x90–0x9F) ===
    FREE_HEAP       = 0x90,   // uint32_t: bytes libres
    WIFI_RSSI       = 0x91,   // int8_t: RSSI actual
    QUEUE_DEPTH     = 0x92,   // uint8_t: paquetes en cola
    TX_COUNT        = 0x93,   // uint32_t: paquetes enviados total
    RX_COUNT        = 0x94,   // uint32_t: paquetes recibidos total
    ACK_TIMEOUTS    = 0x95,   // uint32_t: timeouts de ACK acumulados
    RETRIES_COUNT   = 0x96,   // uint32_t: reintentos acumulados
    BOOT_REASON     = 0x97,   // uint8_t: BootReason enum

    // === State (0xA0–0xAF) ===
    // Reservado para tags de estado definidos por cada aplicación.
    // El core no asigna nombres ni semántica a este rango.

    // === Error (0xE0–0xEF) ===
    ERROR_CODE_TAG  = 0xE0,   // uint8_t: IoTError
    ERROR_SEQ       = 0xE1,   // uint32_t: SEQ del paquete que causó error
    ERROR_DETAIL    = 0xE2,   // string: detalle legible (debug)
};

// ============================================================
// Códigos de error
// ============================================================

enum class IoTError : uint8_t {
    ERR_NONE            = 0x00,
    ERR_BAD_VERSION     = 0x01,
    ERR_BAD_CRC         = 0x02,
    ERR_BAD_LENGTH      = 0x03,
    ERR_UNKNOWN_TYPE    = 0x04,
    ERR_UNKNOWN_DEVICE  = 0x05,
    ERR_INVALID_TLV     = 0x06,
    ERR_QUEUE_FULL      = 0x07,
    ERR_NOT_SUPPORTED   = 0x08,
    ERR_AUTH_FAILED     = 0x09,
    ERR_BUSY            = 0x0A,
    ERR_TIMEOUT         = 0x0B,
};

// ============================================================
// Códigos de resultado para RESPONSE (respuesta a COMMAND)
// ============================================================

enum class ResultCode : uint8_t {
    OK                  = 0x00,   // Comando ejecutado exitosamente
    FAIL                = 0x01,   // Falló la ejecución
    BUSY                = 0x02,   // Dispositivo ocupado, reintentar después
    NOT_SUPPORTED       = 0x03,   // Comando no soportado por este dispositivo
    INVALID_VALUE       = 0x04,   // Valor del comando fuera de rango
    TIMEOUT             = 0x05,   // Actuador no respondió a tiempo
    REJECTED            = 0x06,   // Rechazado por política (ej: modo seguro)
};

// ============================================================
// Razón de reinicio (para diagnóstico)
// ============================================================

enum class BootReason : uint8_t {
    POWER_ON        = 0x00,
    SOFTWARE_RESET  = 0x01,
    WATCHDOG        = 0x02,
    DEEP_SLEEP      = 0x03,
    OTA_UPDATE      = 0x04,
    CRASH           = 0x05,
    UNKNOWN         = 0xFF,
};

// ============================================================
// Estructura del paquete V4.1
// ============================================================

struct IoTPacket {
    // --- Cabecera ---
    uint8_t  version;     // IOT_PROTOCOL_VER (0x41)
    MsgType  type;        // Tipo de mensaje
    uint8_t  src;         // ID origen
    uint8_t  dst;         // ID destino
    uint16_t bootId;      // ID de sesión (random al boot, resuelve reinicios)
    uint32_t seq;         // Número de secuencia (32 bits, nunca rollover en la práctica)
    uint8_t  flags;       // Bitmap de flags

    // --- Payload TLV ---
    uint8_t  payload[IOT_MAX_PAYLOAD];
    uint8_t  payloadLen;  // 1 byte suficiente (max 64)

    // === Escribir TLV ===
    void clearPayload();
    bool addTLV_uint8(TlvTag tag, uint8_t value);
    bool addTLV_int8(TlvTag tag, int8_t value);
    bool addTLV_uint16(TlvTag tag, uint16_t value);
    bool addTLV_int16(TlvTag tag, int16_t value);
    bool addTLV_uint32(TlvTag tag, uint32_t value);
    bool addTLV_string(TlvTag tag, const char* str);

    // === Leer TLV (validación estricta de tamaño) ===
    bool getTLV_uint8(TlvTag tag, uint8_t &value) const;
    bool getTLV_int8(TlvTag tag, int8_t &value) const;
    bool getTLV_uint16(TlvTag tag, uint16_t &value) const;
    bool getTLV_int16(TlvTag tag, int16_t &value) const;
    bool getTLV_uint32(TlvTag tag, uint32_t &value) const;
    bool getTLV_string(TlvTag tag, char* buf, uint8_t maxLen) const;
    bool hasTLV(TlvTag tag) const;

    // === Helpers ===
    bool needsAck() const    { return (flags & IOT_FLAG_ACK_REQUIRED) != 0; }
    bool isReliable() const  { return (flags & IOT_FLAG_RELIABLE) != 0; }
    bool isUrgent() const    { return (flags & IOT_FLAG_URGENT) != 0; }
    bool isBackground() const { return (flags & IOT_FLAG_BACKGROUND) != 0; }

    Priority priority() const {
        if (flags & IOT_FLAG_URGENT) return Priority::URGENT;
        if (flags & IOT_FLAG_BACKGROUND) return Priority::BACKGROUND;
        return Priority::NORMAL;
    }
};

// ============================================================
// CRC16-CCITT
// ============================================================

uint16_t iot_crc16(const uint8_t* data, size_t len);

// ============================================================
// Serialización / Deserialización
// ============================================================

/**
 * Serializa IoTPacket → buffer wire format.
 * @return tamaño total escrito, o 0 si error (buffer insuficiente).
 */
size_t iot_serialize(const IoTPacket &pkt, uint8_t* buf, size_t bufSize);

/**
 * Deserializa buffer → IoTPacket.
 * Validación estricta:
 * - Magic bytes correctos
 * - Versión compatible (major debe coincidir)
 * - Longitud exacta (len == expectedLen)
 * - CRC16 válido
 * @return true si paquete válido, false si cualquier check falla.
 */
bool iot_deserialize(const uint8_t* buf, size_t len, IoTPacket &pkt);

/**
 * Valida la estructura TLV del payload.
 * Recorre todos los TLV verificando que no hay desbordamiento.
 * @return true si payload TLV es estructuralmente válido.
 */
bool iot_validate_tlv(const uint8_t* payload, uint8_t payloadLen);
