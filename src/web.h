// web.h - Web server endpoint registracija za vent_SEW
//
// ARHITEKTURA:
//   setupServer()       (http.cpp) - inicializira AsyncWebServer, kliče setupWebEndpoints()
//   setupWebEndpoints() (web.cpp)  - registrira HTML strani in GET /
//   handleXxx()         (web_handlers.cpp) - implementacija HTML strani
//
// Endpointi po modulu:
//   web.cpp:
//     GET  /           → handleRoot()     - HTML status stran (T, H, IAQ, kamera, baterija)
//     GET  /settings   → handleSettings() - nastavitve enote HTML stran
//     GET  /sd-list    → handleSDList()   - browser SD datotek
//     GET  /sd-file    → handleSDFile()   - download SD datoteke (?name=/path)
//     GET  /logs       → handleLogs()     - RAM log buffer HTML
//   http.cpp:
//     GET  /status        → JSON z aktualnimi vrednostmi
//     GET  /api/ping      → "pong" heartbeat
//     POST /api/settings  → posodobi settings (unitId, IP, offsets, intervali)
//     GET  /api/settings  → vrne aktualne nastavitve
//     POST /api/reset     → reset settings na default
//     GET  /update        → OTA upload HTML
//     POST /update        → OTA firmware flash
//   cam.cpp (via http.cpp setupCameraEndpoints):
//     GET  /cam/status        → JSON stanje kamere
//     POST /cam/record/start  → ročni začetek snemanja
//     POST /cam/record/stop   → ustavitev snemanja
//
// POZOR: setupWebEndpoints() se kliče ZNOTRAJ setupServer() (http.cpp)!
//        main.cpp kliče samo setupServer() - ne kliče setupWebEndpoints() direktno.
//
// POPRAVKI (2026-03-01):
//   - Komentar popravljen: GET / → handleRoot() (prej napačno navajalo redirect)
//     handleRoot() je bila definirana a nikoli registrirana - FIX v web.cpp

#pragma once
#include <Arduino.h>

// Registrira GET strani na server objektu iz http.cpp.
// Kliče se znotraj setupServer() (http.cpp).
void setupWebEndpoints();
