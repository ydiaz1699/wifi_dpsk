#include "device_config.h"

// --- Red ---
IPAddress dispositivo_IP(192, 168, 0, 200);
IPAddress central_IP(192, 168, 0, 201);
const uint16_t UDP_PORT = 4210;

// --- Pines ---
const int PIN_PIR = D2;
const int PIN_TIMBRE = D3;   // INPUT_PULLUP, activo en LOW

// --- Timings ---
const unsigned long ANTIREBOTE_PIR_MS = 200;
const unsigned long ANTIREBOTE_TIMBRE_MS = 800;
const unsigned long HEARTBEAT_INTERVAL_MS = 60000;  // 60s
