// disp_graph.h - LVGL graf za vent_SEW
//
// ARHITEKTURA (graph_update.md, 2026-03-04):
//   - GraphPoint (32 bytes) je definiran tukaj.
//     graph_store.cpp/#include "disp_graph.h" za dostop do strukture.
//   - graphRefresh() bere iz graph_store prek graphGetHistoryOrdered().
//   - Swipe levo/desno = menja sensor, kratek dotik = zoom in, dolg = zoom out.
//   - NVS persistenca: currentGraphSensor in currentGraphHours se shranita
//     ob vsaki spremembi in obnovita ob zagonu.
//
// GRAFI (6 — po graph_update.md §3.4):
//   GRAPH_TEMP  (0) — Temperatura [°C]   SHT41
//   GRAPH_HUM   (1) — Vlažnost [%]       SHT41
//   GRAPH_IAQ   (2) — IAQ 0–500          BSEC2
//   GRAPH_LUX   (3) — Svetloba [lx]      TCS34725
//   GRAPH_WIND  (4) — Veter [km/h]       OpenMeteo
//   GRAPH_CLOUD (5) — Oblačnost [%]      OpenMeteo
//
// ČASOVNA OKNA: 2h → 4h → 8h → 16h → 24h
//
// GRAPH_MOTION je ukinjen (§5 spec: gibanje ostane v detail screenu + REW).

#pragma once
#include <Arduino.h>
#include <lvgl.h>

// -----------------------------------------------------------------------
// Dimenzioniranje (graph_update.md §3.2)
// -----------------------------------------------------------------------
// 24h / 3min = 480 točk za polno zgodovino
#define GRAPH_HISTORY_MAX   480

// Max točk na zaslonu: 1 točka = 1 piksel pri 240px širini grafa
#define DISPLAY_POINTS      240

// -----------------------------------------------------------------------
// GraphPoint — podatkovna točka v zgodovini (32 bytes, graph_update.md §3.3)
// Podatki se berejo iz sensorData + weatherData ob vsakem 3-min ciklu.
// -----------------------------------------------------------------------
struct GraphPoint {
    uint32_t ts;     // Unix timestamp [s]          (4 bytes)
    float    temp;   // Temperatura [°C]             (4 bytes)
    float    hum;    // Relativna vlažnost [%]       (4 bytes)
    float    press;  // Atmosferski tlak [hPa]       (4 bytes)
    float    iaq;    // IAQ indeks [0–500]           (4 bytes)
    float    lux;    // Osvetljenost [lx]            (4 bytes)
    float    wind;   // Hitrost vetra [km/h]         (4 bytes)  ← OpenMeteo
    float    cloud;  // Pokritost oblakov [%]        (4 bytes)  ← OpenMeteo
    // Skupaj: 32 bytes
};

// -----------------------------------------------------------------------
// Tipi grafov (6 — graph_update.md §3.4)
// GRAPH_MOTION ukinjen: motion ostane v sensorData (detail screen, REW).
// -----------------------------------------------------------------------
enum GraphSensor {
    GRAPH_TEMP  = 0,   // Temperatura [°C]    — SHT41
    GRAPH_HUM   = 1,   // Vlažnost [%]        — SHT41
    GRAPH_IAQ   = 2,   // IAQ [0–500]         — BSEC2
    GRAPH_LUX   = 3,   // Svetloba [lx]       — TCS34725
    GRAPH_WIND  = 4,   // Veter [km/h]        — OpenMeteo
    GRAPH_CLOUD = 5,   // Oblačnost [%]       — OpenMeteo
    GRAPH_COUNT = 6
};

// -----------------------------------------------------------------------
// Globalne spremenljivke (definirane v disp_graph.cpp)
// -----------------------------------------------------------------------
extern int currentGraphSensor;   // trenutno prikazan sensor (0..GRAPH_COUNT-1)
// currentGraphHours je v globals.h (extern int currentGraphHours)

// -----------------------------------------------------------------------
// API
// -----------------------------------------------------------------------

// Inicializacija: ustvari LVGL widgete, registrira event handler
// Kliči enkrat iz initDisplay()
void initGraph(lv_obj_t* parent, int x, int y, int w, int h);

// Osveži LVGL graf z aktualnimi podatki iz graph_store
// Kliči iz 3-minutne zanke (main.cpp) in ob spremembi sensor/zoom
void graphRefresh();

// Naložita/shranita currentGraphSensor in currentGraphHours v NVS
void loadGraphPrefs();
void saveGraphPrefs();

// Pomožne funkcije (sensor metapodatki)
const char* graphSensorName(int idx);
const char* graphSensorUnit(int idx);
lv_color_t  graphSensorColor(int idx);
