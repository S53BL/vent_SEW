// http.h - HTTP communication module for vent_SEW
//
// Naloga modula:
//   1. sendToREW()   - pošlje JSON z vsemi senzorskimi podatki na REW /data
//   2. setupServer() - zažene AsyncWebServer z vsemi endpointi:
//        GET  /                  -> preusmeritev na /status
//        GET  /status            -> JSON z aktualnimi vrednostmi
//        GET  /api/ping          -> "pong" heartbeat
//        POST /api/settings      -> posodobitev settings (unitId, IP, offsets, intervali)
//        GET  /api/settings      -> vrne aktualne nastavitve
//        POST /api/reset         -> reset settings na privzete vrednosti
//        GET  /update            -> OTA upload HTML
//        POST /update            -> OTA firmware flash
//        GET  /cam/status        -> JSON stanje kamere (via setupCameraEndpoints)
//        POST /cam/record/start  -> ročni začetek snemanja
//        POST /cam/record/stop   -> ustavitev snemanja
//        GET  /                  -> redirect /status (via setupWebEndpoints)
//        GET  /settings          -> nastavitve HTML stran
//        GET  /sd-list           -> JSON seznam SD datotek
//        GET  /sd-file           -> vsebina SD datoteke
//        GET  /logs              -> sistemski logi HTML
//   3. performREWPingCheck() - preveri dosegljivost REW
//   4. setupCameraEndpoints() - registrira kamera endpointe (klicano iz setupServer)
//
// Vrstni red registracije endpointov v setupServer():
//   1. setupCameraEndpoints()  - /cam/*
//   2. setupWebEndpoints()     - /settings, /sd-list, /sd-file, /logs
//   3. Interni endpointi       - /status, /api/*, /update, OTA
//   4. server.begin()
//
// JSON format ki ga SEW pošlje na REW POST /data:
//   {
//     "id":      "SEW1",
//     "temp":    12.34,
//     "hum":     65.1,
//     "press":   1013.2,
//     "iaq":     85,
//     "iaq_acc": 3,
//     "siaq":    82,
//     "eco2":    650.0,
//     "bvoc":    0.52,
//     "lux":     234.5,
//     "cct":     5500,
//     "r":       1024, "g": 890, "b": 760,
//     "motion":  true,
//     "mcnt":    3,
//     "bat":     3.85,
//     "bat_pct": 72,
//     "err":     6,
//     "ts":      1708600000
//   }
//
// REW pricakuje identifikacijo enote prek polja "id" - tako razlikuje med SEW1, SEW2...
// Neznana polja REW ignorira (prihodnja kompatibilnost).

#ifndef HTTP_H
#define HTTP_H

#include "globals.h"
#include <ESPAsyncWebServer.h>

extern AsyncWebServer server;

// Inicializacija HTTP streznika + vsi endpointi
// Interno kliče: setupCameraEndpoints(), setupWebEndpoints(), nato server.begin()
bool setupServer();

// Registrira kamera endpointe (/cam/status, /cam/record/start, /cam/record/stop)
// MORA biti klicana PRED server.begin() - kliče jo setupServer()
void setupCameraEndpoints();

// Pošlje podatke na REW (klic iz main loop)
// Vrne HTTP kodo (200=OK, -1=connection error, -2=WiFi not connected, -3=backoff)
int sendToREW();

// Preveri dosegljivost REW prek GET /api/ping
bool performREWPingCheck();

// Sestavi JSON payload (za debug / logging)
String buildPayloadJSON();

#endif // HTTP_H
