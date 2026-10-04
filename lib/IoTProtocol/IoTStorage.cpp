/**
 * IoTStorage V4.3 — Implementación LittleFS
 *
 * Usa LittleFS (incluido en ESP8266 Arduino core) para persistir:
 * - Boot counter: archivo simple con un número
 * - Configuración: archivo key=value con CRC16 de integridad
 * - Auth key: archivo binario
 */

#include "IoTStorage.h"
#include <LittleFS.h>
#include <stdlib.h>
#include <string.h>

// Paths en el filesystem
static const char* PATH_BOOT_COUNT = "/iot/boot_count";
static const char* PATH_CONFIG     = "/iot/config.json";
static const char* PATH_CONFIG_TMP = "/iot/config.json.tmp";
static const char* PATH_AUTH_KEY   = "/iot/auth.key";
static const uint8_t STORAGE_MOUNT_ATTEMPTS = 2;

// ============================================================
// Constructor
// ============================================================

IoTStorage::IoTStorage()
    : _bootCount(0), _mounted(false), _bootCounterValid(false),
      _bootIdPersistent(false), _bootIdConsumed(false), _mountAttempts(0)
{
    _setDefaults();
}

// ============================================================
// Inicialización
// ============================================================

bool IoTStorage::_mountLittleFS() {
    if (!LittleFS.begin()) return false;

    if (!LittleFS.exists("/iot") && !LittleFS.mkdir("/iot")) {
        return false;
    }

    _mounted = true;
    _bootCounterValid = _readBootCount();
    _bootIdPersistent = _bootCounterValid && !_bootIdConsumed;
    return true;
}

bool IoTStorage::begin() {
    _mountAttempts = 0;
    for (uint8_t attempt = 0; attempt < STORAGE_MOUNT_ATTEMPTS; ++attempt) {
        ++_mountAttempts;
        if (_mountLittleFS()) return true;
    }

    _mounted = false;
    _bootCounterValid = false;
    _bootIdPersistent = false;
    return false;
}

bool IoTStorage::retryMount() {
    if (_mounted) return true;

    _mountAttempts = 0;
    for (uint8_t attempt = 0; attempt < STORAGE_MOUNT_ATTEMPTS; ++attempt) {
        ++_mountAttempts;
        if (_mountLittleFS()) {
            // El BOOT_ID se decide una sola vez por arranque. Si ya se
            // consumió el fallback, no anunciar persistencia retrospectiva.
            if (_bootIdConsumed) _bootIdPersistent = false;
            return true;
        }
    }

    _mounted = false;
    _bootCounterValid = false;
    _bootIdPersistent = false;
    return false;
}

// ============================================================
// Boot Counter → BOOT_ID
// ============================================================

uint16_t IoTStorage::getBootId() {
    _bootIdConsumed = true;
    if (!_mounted || !_bootCounterValid) {
        _bootIdPersistent = false;
        return _volatileBootId();
    }

    if (_bootCount == 0xFFFFFFFFUL) {
        _bootCount = 0;
    }
    ++_bootCount;

    if (!_writeBootCount()) {
        // No anunciar una sesión persistente si el contador no llegó al FS.
        _bootIdPersistent = false;
        return _volatileBootId();
    }

    _bootIdPersistent = true;
    // Wrapping: nunca usar 0.
    uint16_t id = (uint16_t)(_bootCount & 0xFFFF);
    if (id == 0) id = 1;
    return id;
}

uint16_t IoTStorage::_volatileBootId() const {
    // Opción A del brief: conservar operación degradada, pero hacerla
    // explícita. Este valor no garantiza unicidad entre reinicios.
    uint16_t id = static_cast<uint16_t>(micros()) ^
                  static_cast<uint16_t>(analogRead(A0) << 8);
    if (id == 0) id = 1;
    return id;
}

bool IoTStorage::_readBootCount() {
    if (!LittleFS.exists(PATH_BOOT_COUNT)) {
        _bootCount = 0;
        return true;
    }

    File f = LittleFS.open(PATH_BOOT_COUNT, "r");
    if (!f) return false;

    String line = f.readStringUntil('\n');
    f.close();
    line.trim();
    if (line.length() == 0) return false;

    for (unsigned int i = 0; i < line.length(); ++i) {
        const char c = line.charAt(i);
        if (c < '0' || c > '9') return false;
    }

    const long parsed = line.toInt();
    if (parsed < 0) return false;
    _bootCount = static_cast<uint32_t>(parsed);
    return true;
}

bool IoTStorage::_writeBootCount() {
    File f = LittleFS.open(PATH_BOOT_COUNT, "w");
    if (!f) return false;

    f.println(_bootCount);
    f.close();
    return true;
}

// ============================================================
// Configuración — formato key=value con CRC16
// (Sin ArduinoJson para no agregar dependencia extra a la lib)
// Formato simple: key=value por línea
// ============================================================

bool IoTStorage::loadConfig() {
    _setDefaults();
    // Esquema V1 estricto: se valida el archivo completo antes de copiar
    // `candidate` a `_config`. No existe migración automática por
    // configVersion; agregar/quitar claves requiere implementarla aquí y
    // probarla antes de desplegar una nueva versión.
    if (!_mounted || !LittleFS.exists(PATH_CONFIG)) {
        return false;
    }

    File f = LittleFS.open(PATH_CONFIG, "r");
    if (!f) return false;

    IoTConfig candidate = _config;
    String canonical;
    uint16_t seen = 0;
    uint16_t expectedChecksum = 0;
    bool checksumSeen = false;
    bool valid = true;

    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) {
            valid = false;
            break;
        }

        const int sep = line.indexOf('=');
        if (sep < 0) {
            valid = false;
            break;
        }

        const String key = line.substring(0, sep);
        const String val = line.substring(sep + 1);
        if (key == "checksum") {
            // El checksum debe ser la última línea y no forma parte de sí
            // mismo; cualquier dato posterior invalida el archivo.
            if (checksumSeen) {
                valid = false;
                break;
            }
            expectedChecksum = static_cast<uint16_t>(val.toInt());
            checksumSeen = true;
            continue;
        }
        if (checksumSeen) {
            valid = false;
            break;
        }

        uint16_t bit = 0;
        if (key == "name") {
            bit = 1U << 0;
            val.toCharArray(candidate.deviceName, IOT_DEVICE_NAME_MAX);
        } else if (key == "id") {
            bit = 1U << 1;
            candidate.deviceId = (uint8_t)val.toInt();
        } else if (key == "central_ip") {
            bit = 1U << 2;
            if (!candidate.centralIP.fromString(val)) valid = false;
        } else if (key == "udp_port") {
            bit = 1U << 3;
            candidate.udpPort = (uint16_t)val.toInt();
        } else if (key == "heartbeat_ms") {
            bit = 1U << 4;
            candidate.heartbeatIntervalMs = (uint32_t)val.toInt();
        } else if (key == "antirebote_ms") {
            bit = 1U << 5;
            candidate.antireboteMs = (uint32_t)val.toInt();
        } else if (key == "auth_enabled") {
            bit = 1U << 6;
            candidate.authEnabled = (val == "1" || val == "true");
        } else if (key == "auth_key_len") {
            bit = 1U << 7;
            candidate.authKeyLen = (uint8_t)val.toInt();
        } else if (key == "config_version") {
            bit = 1U << 8;
            candidate.configVersion = (uint8_t)val.toInt();
        } else {
            valid = false;
            break;
        }

        if (seen & bit) {
            valid = false;
            break;
        }
        seen |= bit;
        canonical += line;
        canonical += '\n';
    }
    f.close();

    const uint16_t required = 0x01FF;
    if (!valid || !checksumSeen || seen != required) {
        _setDefaults();
        return false;
    }

    const uint16_t actualChecksum = iot_crc16(
        reinterpret_cast<const uint8_t*>(canonical.c_str()), canonical.length());
    if (actualChecksum != expectedChecksum) {
        _setDefaults();
        return false;
    }

    _config = candidate;
    return true;
}

bool IoTStorage::saveConfig() {
    if (!_mounted) return false;

    String content;
    content += "name=";
    content += _config.deviceName;
    content += '\n';
    content += "id=";
    content += String(_config.deviceId);
    content += '\n';
    content += "central_ip=";
    content += _config.centralIP.toString();
    content += '\n';
    content += "udp_port=";
    content += String(_config.udpPort);
    content += '\n';
    content += "heartbeat_ms=";
    content += String(_config.heartbeatIntervalMs);
    content += '\n';
    content += "antirebote_ms=";
    content += String(_config.antireboteMs);
    content += '\n';
    content += "auth_enabled=";
    content += String(_config.authEnabled ? 1 : 0);
    content += '\n';
    content += "auth_key_len=";
    content += String(_config.authKeyLen);
    content += '\n';
    content += "config_version=";
    content += String(_config.configVersion);
    content += '\n';

    const uint16_t checksum = iot_crc16(
        reinterpret_cast<const uint8_t*>(content.c_str()), content.length());

    // Nunca truncar el archivo final: el contenido completo se prepara en un
    // temporal y solo después se sustituye con rename() atómico de LittleFS.
    LittleFS.remove(PATH_CONFIG_TMP);
    File f = LittleFS.open(PATH_CONFIG_TMP, "w");
    if (!f) return false;
    f.print(content);
    f.printf("checksum=%u\n", checksum);
    f.close();

    if (!LittleFS.rename(PATH_CONFIG_TMP, PATH_CONFIG)) {
        LittleFS.remove(PATH_CONFIG_TMP);
        return false;
    }
    return true;
}

void IoTStorage::resetConfig() {
    _setDefaults();
    if (_mounted) {
        LittleFS.remove(PATH_CONFIG);
        LittleFS.remove(PATH_AUTH_KEY);
    }
}

void IoTStorage::_setDefaults() {
    _config = IoTConfig{};
    strncpy(_config.deviceName, "IoT Device", IOT_DEVICE_NAME_MAX);
    _config.deviceId = 0x02;
    _config.centralIP = IPAddress(192, 168, 0, 201);
    _config.udpPort = 4210;
    _config.heartbeatIntervalMs = 60000;
    _config.antireboteMs = 200;
    _config.authEnabled = false;
    _config.authKeyLen = 0;
    _config.configVersion = 1;
}

// ============================================================
// Auth Key
// ============================================================

bool IoTStorage::loadAuthKey() {
    if (!_mounted || !LittleFS.exists(PATH_AUTH_KEY)) return false;

    File f = LittleFS.open(PATH_AUTH_KEY, "r");
    if (!f) return false;

    size_t bytesRead = f.read(_config.authKey, IOT_STORAGE_KEY_MAX);
    f.close();

    if (bytesRead > 0 && bytesRead <= IOT_STORAGE_KEY_MAX) {
        _config.authKeyLen = (uint8_t)bytesRead;
        return true;
    }
    return false;
}

bool IoTStorage::saveAuthKey(const uint8_t* key, uint8_t len) {
    if (!_mounted) return false;
    if (len > IOT_STORAGE_KEY_MAX) len = IOT_STORAGE_KEY_MAX;

    File f = LittleFS.open(PATH_AUTH_KEY, "w");
    if (!f) return false;

    f.write(key, len);
    f.close();

    memcpy(_config.authKey, key, len);
    _config.authKeyLen = len;
    return true;
}

// ============================================================
// Utilidades
// ============================================================

bool IoTStorage::format() {
    // format() es una operación explícita y destructiva. Dejar el estado
    // interno degradado hasta confirmar que el volumen vuelve a montar evita
    // anunciar un filesystem utilizable si begin() posterior falla.
    _mounted = false;
    _bootCounterValid = false;
    _bootIdPersistent = false;

    if (!LittleFS.format()) return false;
    if (!LittleFS.begin()) return false;
    if (!LittleFS.exists("/iot") && !LittleFS.mkdir("/iot")) return false;

    _setDefaults();
    _bootCount = 0;
    _bootCounterValid = true;
    _mounted = true;
    _bootIdPersistent = !_bootIdConsumed;
    return true;
}

uint32_t IoTStorage::freeSpace() const {
    if (!_mounted) return 0;
    FSInfo info;
    LittleFS.info(info);
    return info.totalBytes - info.usedBytes;
}
