// web.cpp - Web UI endpointi za vent_SEW
//
// Registrira HTML strani in SD file browser.
// Klicana iz http.cpp setupServer() PRED server.begin().
//
// Endpointi:
//   GET /           -> HTML status stran (handleRoot)        ← FIX 2026-03-01
//   GET /settings   -> Nastavitve HTML stran (handleSettings)
//   GET /sd-list    -> SD datoteke browser (handleSDList)
//   GET /sd-file    -> Vsebina datoteke z SD (handleSDFile)
//   GET /logs       -> Sistemski logi HTML (handleLogs)
//
// OPOMBA: /api/ping in /api/reset sta definirani v http.cpp setupServer()!
//   Tukaj ju NE registriramo - dvojna registracija pri ESPAsyncWebServer
//   povzroča nedefiniran vrstni red odgovorov.
//
// POPRAVKI (2026-03-01):
//   - Odstranjeni duplikati /api/ping in /api/reset
//   - setupWebEndpoints() klicana iz http.cpp setupServer() PRED server.begin()
//
// POPRAVKI (2026-03-01 v2):
//   - FIX KRITIČNO: handleRoot() je bila definirana v web_handlers.cpp
//     a NIKOLI registrirana na noben URL!
//     http.cpp je imel "GET /" → 302 redirect na "/status" (JSON endpoint),
//     kar pomeni: brskalnik → http://192.168.2.191/ → JSON, ne HTML!
//     Zdaj: GET / → handleRoot() (HTML status stran z vsemi metrikami)
//     http.cpp "GET /" redirect je bil odstranjen - ta modul prevzame GET /

#include "web.h"
#include "web_handlers.h"
#include "globals.h"
#include "logging.h"
#include <ESPAsyncWebServer.h>

extern AsyncWebServer server;

void setupWebEndpoints() {
    // FIX (2026-03-01): handleRoot() definirana v web_handlers.cpp
    // ni bila nikoli registrirana! Brskalnik je dobil JSON (redirect na /status),
    // ne HTML stran. Zdaj GET / → celotna HTML status stran.
    server.on("/",         HTTP_GET, handleRoot);

    // HTML strani - vizualni web vmesnik
    server.on("/settings", HTTP_GET, handleSettings);
    server.on("/sd-list",  HTTP_GET, handleSDList);
    server.on("/sd-file",  HTTP_GET, handleSDFile);
    server.on("/logs",     HTTP_GET, handleLogs);
    server.on("/motion",   HTTP_GET, handleMotion);
    server.on("/graphs",   HTTP_GET, handleGraphs);
    server.on("/api/delete-files", HTTP_POST, handleDeleteFiles);
    server.on("/api/graph-data",   HTTP_GET,  handleGraphData);

    // /api/ping in /api/reset sta registrirani v http.cpp setupServer() - ne podvajamo!

    LOG_INFO("WEB", "Web UI endpoints registered: /, /settings, /sd-list, /sd-file, /logs, /motion, /graphs, /api/delete-files, /api/graph-data");
}
