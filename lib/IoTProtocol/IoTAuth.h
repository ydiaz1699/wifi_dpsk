/**
 * IoTAuth V4.3 — Autenticación HMAC-SHA256 truncada para IoTProtocol
 *
 * Proporciona autenticación de paquetes usando HMAC-SHA256 truncado
 * a 4 bytes. Suficiente para prevenir inyección casual en LAN WiFi.
 *
 * Funcionamiento:
 * - Se calcula HMAC-SHA256(key, header + payload) → 32 bytes
 * - Se trunca a los primeros 4 bytes
 * - Se agrega como TLV tag AUTH_HMAC4 al payload antes de serializar
 * - El receptor verifica el HMAC antes de procesar
 *
 * Uso recomendado en V4:
 *   IoTAuth auth(key, keyLen);
 *   IoTAuthProvider provider{IoTAuthMode::REQUIRED, true, verifyCb,
 *                            signCb, nullptr, &auth};
 *   node.setAuthProvider(provider);
 *
 * `signPacket()` es idempotente para que IoTNode pueda ser la única frontera
 * de firma aunque callers legacy entreguen un paquete ya firmado.
 *
 * Uso directo (compatibilidad):
 *   auth.signPacket(pkt);     // Agrega AUTH_HMAC4 al payload
 *
 * En recepción, la verificación debe configurarse en IoTNode; no repetirla
 * dentro del handler, porque sería posterior a registry/ACK/deduplicación.
 * Seguridad:
 * - Previene: vecino inyectando paquetes falsos
 * - NO previene: replay (ya cubierto por SEQ + BOOT_ID + dedup window)
 * - NO cifra: el contenido es visible (confidencialidad no es objetivo V4.3)
 * - 4 bytes = 2^32 combinaciones, suficiente para brute-force prevention en LAN
 *
 * El flag IOT_FLAG_AUTHENTICATED (0x10) indica que el paquete tiene HMAC.
 * Si auth está habilitado en el receptor y el paquete no tiene el flag → rechazado.
 */

#pragma once
#include "IoTProtocol.h"

// Flag adicional para autenticación se define en IoTProtocol.h para que
// IoTNode pueda aplicar la política antes de cualquier efecto de recepción.
// TLV tag para el HMAC truncado (4 bytes)
// Definido en rango 0xF0–0xFF (seguridad)
#define IOT_TLV_AUTH_HMAC4  0xF0

// Tamaño del HMAC truncado
#define IOT_HMAC_TRUNC_SIZE  4

// Tamaño máximo de la clave compartida
#define IOT_AUTH_KEY_MAX_SIZE  32

class IoTAuth {
public:
    /**
     * Constructor.
     * @param key    Clave compartida (shared secret)
     * @param keyLen Largo de la clave (recomendado: 16 bytes)
     */
    IoTAuth(const uint8_t* key, uint8_t keyLen);

    /**
     * Firma un paquete: calcula HMAC y agrega TLV AUTH_HMAC4 al payload.
     * Debe llamarse DESPUÉS de agregar todos los TLV del usuario.
     * Setea el flag IOT_FLAG_AUTHENTICATED.
     * @return true si se pudo agregar (hay espacio en payload)
     */
    bool signPacket(IoTPacket &pkt);

    /**
     * Verifica un paquete: extrae AUTH_HMAC4, recalcula HMAC y compara.
     * @return true si el HMAC es válido
     */
    bool verifyPacket(const IoTPacket &pkt);

    /**
     * ¿El paquete tiene el flag de autenticación?
     */
    static bool isAuthenticated(const IoTPacket &pkt) {
        return (pkt.flags & IOT_FLAG_AUTHENTICATED) != 0;
    }

    /**
     * Habilitar/deshabilitar rechazo de paquetes no autenticados
     */
    void setRequired(bool required) { _required = required; }
    bool isRequired() const { return _required; }

private:
    uint8_t _key[IOT_AUTH_KEY_MAX_SIZE];
    uint8_t _keyLen;
    bool    _required;

    /**
     * Calcula HMAC-SHA256 truncado a 4 bytes sobre los datos relevantes
     * del paquete (header fields + payload sin el TLV de auth).
     */
    void _computeHmac(const IoTPacket &pkt, uint8_t payloadLenForHash, uint8_t out[IOT_HMAC_TRUNC_SIZE]);
};
