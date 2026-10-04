#pragma once
#include <Arduino.h>

// Endpoint HTTP de emergencia para nodos satélite ESPHome.
//
// Ruta:  POST /event
// Body:  { "v":1, "src":66, "boot":1, "seq":42, "code":2, "val":1,
//          "name":"Ventana Cocina", "dev_type":7 }
// Respuestas:
//   200 OK        -> evento aceptado y despachado al handler
//   409 Conflict  -> duplicado (misma src/boot/seq ya vista)
//   400 Bad Req.  -> JSON malformado o campos faltantes
//   403 Forbidden -> política auth bloquea el canal HTTP, o replay fuera de ventana
//   404 Not Found -> ruta desconocida
//
// Este endpoint es un canal DEGRADADO: no aplica HMAC (el wire HMAC cubre
// el header binario, no JSON). Si `authEnabled == true` en el receptor,
// el endpoint rechaza todas las peticiones con 403 por diseño.

void setupHttpEndpoint();
void loopHttpEndpoint();
