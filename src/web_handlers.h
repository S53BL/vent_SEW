// web_handlers.h - HTTP handler deklaracije za vent_SEW
//
// Implementacija: web_handlers.cpp
// Registracija na server: web.cpp → setupWebEndpoints()
// Klic: setupServer() (http.cpp) → setupWebEndpoints() (web.cpp)

#pragma once
#include <ESPAsyncWebServer.h>

// GET strani
void handleRoot(AsyncWebServerRequest* request);
void handleSettings(AsyncWebServerRequest* request);
void handleSDList(AsyncWebServerRequest* request);
void handleSDFile(AsyncWebServerRequest* request);
void handleLogs(AsyncWebServerRequest* request);
void handleDeleteFiles(AsyncWebServerRequest* request);
void handleMotion(AsyncWebServerRequest* request);
void handleGraphs(AsyncWebServerRequest* request);
void handleGraphData(AsyncWebServerRequest* request);
