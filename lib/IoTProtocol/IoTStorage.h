/**
 * IoTStorage V4.3 — Persistencia LittleFS para IoTProtocol
 *
 * Proporciona:
 * - Boot counter persistente (reemplaza random BOOT_ID → determinístico e incremental)
 * - Almacenamiento de configuración del dispositivo (key=value + CRC16)
 * - Clave de autenticación persistente
 *
 * Archivos en LittleFS:
 *   /iot/boot_count    — uint32_t en texto (1 línea)
 *   /iot/config.json   — configuración key=value + checksum CRC16
 *   /iot/auth.key      — clave HMAC (binario, 16-32 bytes)
 *
 * Compatibilidad de esquema:
 *   config.json mantiene un nombre histórico, pero su contenido actual es
 *   key=value con CRC16. El esquema V1 exige las nueve claves conocidas y
 *   no migra automáticamente archivos antiguos o futuros. Un cambio de
 *   esquema requiere una rutina de migración explícita antes del despliegue.
 *   auth.key es independiente de auth_key_len; las aplicaciones actuales
 *   usan IOT_AUTH_KEY desde secrets.h, salvo que se cambie explícitamente
 *   el firmware para cargar la clave persistente.
 *
 * Uso:
 *   IoTStorage storage;
 *   storage.begin();
 *   uint16_t bootId = storage.getBootId();  // Incrementa y persiste
 *
 *   // Leer/escribir config
 *   storage.setDeviceName("PIR Entrada");
 *   storage.setCentralIP(IPAddress(192,168,0,201));
 *   storage.save();
 *
 * Nota: LittleFS debe formatearse explícitamente una vez en primera
 * instalación. begin() reintenta el montaje sin formatear ni borrar datos;
 * format() solo se ejecuta por una operación explícita.
 */

#pragma once
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include "IoTProtocol.h"

// Tamaños máximos
#define IOT_STORAGE_KEY_MAX      32

// Configuración persistente
struct IoTConfig {
    // Identidad
    char     deviceName[IOT_DEVICE_NAME_MAX];
    uint8_t  deviceId;

    // Red
    IPAddress centralIP;
    uint16_t  udpPort;

    // Timing
    uint32_t heartbeatIntervalMs;
    uint32_t antireboteMs;

    // Auth
    uint8_t  authKey[IOT_STORAGE_KEY_MAX];
    uint8_t  authKeyLen;
    bool     authEnabled;

    // Versión de config (para detectar cambios)
    // La versión actual es 1. loadConfig() exige el esquema completo y no
    // migra versiones automáticamente: cualquier cambio de claves requiere
    // una rutina de migración explícita antes de desplegar el firmware.
    uint8_t  configVersion;
};

class IoTStorage {
public:
    IoTStorage();

    /**
     * Inicializar LittleFS. Reintenta el montaje sin formatear.
     * @return true si montó correctamente
     */
    bool begin();

    /**
     * Reintenta montar LittleFS después de un fallo transitorio.
     * No cambia el BOOT_ID ya consumido ni recarga configuración en runtime.
     */
    bool retryMount();

    /** Indica si LittleFS está montado en este momento. */
    bool isMounted() const { return _mounted; }

    /**
     * Obtiene el BOOT_ID actual (incrementa el counter y persiste).
     * Llamar una vez al boot. Es uint16_t (wraps en 65535 → 1).
     * Si LittleFS o el contador fallan, devuelve un ID volátil no cero y
     * `isBootIdPersistent()` queda en false.
     */
    uint16_t getBootId();

    /**
     * Indica si el BOOT_ID devuelto por getBootId() quedó persistido.
     * false significa modo degradado: la deduplicación entre reinicios no
     * puede garantizar una nueva sesión.
     */
    bool isBootIdPersistent() const { return _bootIdPersistent; }

    /**
     * Lee el boot counter sin incrementar.
     */
    uint32_t getBootCount() const { return _bootCount; }

    // --- Configuración ---

    /**
     * Carga la configuración desde /iot/config.json.
     * Si no existe, usa valores por defecto.
     */
    bool loadConfig();

    /**
     * Guarda la configuración actual a /iot/config.json.
     */
    bool saveConfig();

    /**
     * Resetear configuración a valores por defecto.
     */
    void resetConfig();

    /**
     * Acceso a la config actual (lectura/escritura directa).
     */
    IoTConfig& config() { return _config; }
    const IoTConfig& config() const { return _config; }

    // --- Auth key ---

    /**
     * Carga la clave de /iot/auth.key.
     * @return true si hay clave guardada
     */
    bool loadAuthKey();

    /**
     * Guarda la clave a /iot/auth.key.
     */
    bool saveAuthKey(const uint8_t* key, uint8_t len);

    // --- Utilidades ---

    /**
     * Formatea LittleFS (borra todo).
     */
    bool format();

    /**
     * Espacio libre en LittleFS (bytes).
     */
    uint32_t freeSpace() const;

private:
    uint32_t _bootCount;
    IoTConfig _config;
    bool _mounted;
    bool _bootCounterValid;
    bool _bootIdPersistent;
    bool _bootIdConsumed;
    uint8_t _mountAttempts;

    bool _mountLittleFS();
    bool _readBootCount();
    bool _writeBootCount();
    uint16_t _volatileBootId() const;
    void _setDefaults();
};
