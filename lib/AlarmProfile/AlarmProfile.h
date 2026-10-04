/**
 * AlarmProfile — vocabulario específico de alarma doméstica.
 *
 * Este archivo pertenece a la aplicación de alarma, no al core genérico
 * IoTProtocol. Un robot, gateway u otra aplicación debe definir su propio
 * perfil de dominio y conservar sus identificadores wire por separado.
 *
 * Los valores numéricos son parte del contrato wire existente. No cambiar,
 * reutilizar ni renumerar estos valores sin versionar explícitamente el
 * contrato de aplicación.
 */
#pragma once

#include <stdint.h>
#include <IoTProtocol.h>

namespace AlarmProfile {

enum class EventCode : uint8_t {
    MOTION          = 0x01,
    DOOR_OPEN       = 0x02,
    DOOR_CLOSE      = 0x03,
    BUTTON_PRESS    = 0x04,
    BUTTON_RELEASE  = 0x05,
    TIMBRE          = 0x06,
    SMOKE           = 0x07,
    FLOOD           = 0x08,
    TAMPER          = 0x09,
    LOW_BATTERY     = 0x0A,
    WINDOW_OPEN     = 0x0B,
    WINDOW_CLOSE    = 0x0C,
    VIBRATION       = 0x0D,
    GAS_DETECTED    = 0x0E,
};

enum class DeviceType : uint8_t {
    CENTRAL         = 0x01,
    PIR_SENSOR      = 0x02,
    BUTTON          = 0x03,
    TEMP_SENSOR     = 0x04,
    RELAY           = 0x05,
    DISPLAY_DEV     = 0x06,
    DOOR_SENSOR     = 0x07,
    SMOKE_SENSOR    = 0x08,
    MULTI_SENSOR    = 0x09,
    HUMIDITY_SENSOR = 0x0A,
    FLOOD_SENSOR    = 0x0B,
    GAS_SENSOR      = 0x0C,
};

// Tags de estado de aplicación: conservan el rango 0xA0–0xA6 del wire.
enum class StateTag : uint8_t {
    STATE_MOTION = 0xA0,
    STATE_DOOR   = 0xA1,
    STATE_RELAY  = 0xA2,
    STATE_BUTTON = 0xA3,
    STATE_ALARM  = 0xA4,
    STATE_SMOKE  = 0xA5,
    STATE_FLOOD  = 0xA6,
};

constexpr uint8_t toWire(EventCode code) {
    return static_cast<uint8_t>(code);
}

constexpr uint8_t toWire(DeviceType type) {
    return static_cast<uint8_t>(type);
}

constexpr TlvTag toCoreTlvTag(StateTag tag) {
    return static_cast<TlvTag>(static_cast<uint8_t>(tag));
}

}  // namespace AlarmProfile
