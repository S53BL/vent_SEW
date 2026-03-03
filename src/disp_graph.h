// disp_graph.h - LVGL graf za vent_SEW
//
// ARHITEKTURA (korak 3, 2026-03-03):
//   - GraphPoint in history buffer sta ODSTRANJENI iz tega modula.
//     Zgodovino hrani graph_store.cpp (LittleFS ring buffer, gsHistory[]).
//   - graphRefresh() bere iz gsHistory[] prek graphStoreGet() glede na
//     currentGraphHours časovno okno.
//   - Swipe levo/desno = menja sensor, kratek dotik = zoom in, dolgi = zoom out.
//   - NVS persistenca: currentGraphSensor in currentGraphHours se shranita
//     ob vsaki spremembi in obnovita ob zagonu.
//
// GRAFI (5):
//   GRAPH_TEMP (0)  — Temperatura [°C]   SHT41
//   GRAPH_HUM  (1)  — Vlažnost [%]        SHT41
//   GRAPH_IAQ  (2)  — IAQ 0–500           BSEC
//   GRAPH_MOTION(3) — Gibanje [dogodki/h] PIR  → stolpčni graf
//   GRAPH_WIND (4)  — Veter [km/h]        OpenMeteo
//
// ČASOVNA OKNA: 2h → 4h → 8h → 16h → 24h

#pragma once
#include <Arduino.h>
#include <lvgl.h>

// -----------------------------------------------------------------------
// Tipi grafov (5 — skladno s specifikacijo grafi_ekran_SEW.md)
// -----------------------------------------------------------------------
enum GraphSensor {
    GRAPH_TEMP   = 0,
    GRAPH_HUM    = 1,
    GRAPH_IAQ    = 2,
    GRAPH_MOTION = 3,
    GRAPH_WIND   = 4,
    GRAPH_COUNT  = 5
};

// -----------------------------------------------------------------------
// Globalne spremenljivke (definirane v disp_graph.cpp)
// -----------------------------------------------------------------------
extern int currentGraphSensor;   // trenutno prikazan sensor (0..4)
// currentGraphHours je v globals.h (extern int currentGraphHours)

// -----------------------------------------------------------------------
// API
// -----------------------------------------------------------------------

// Inicializacija: ustvari LVGL widgete, registrira event handler
// Kliči enkrat iz initDisplay() ali setup()
void initGraph(lv_obj_t* parent, int x, int y, int w, int h);

// Osveži LVGL graf z aktualnimi podatki iz graph_store
// Kliči iz glavne 3-minutne zanke (main.cpp) in ob spremembi sensor/zoom
void graphRefresh();

// Naložita/shranita currentGraphSensor in currentGraphHours v NVS
void loadGraphPrefs();
void saveGraphPrefs();

// Pomožne funkcije (uporabljene tudi v disp.cpp za statusno vrstico)
const char* graphSensorName(int idx);
const char* graphSensorUnit(int idx);
lv_color_t  graphSensorColor(int idx);

// graphAddPoint() je ODSTRANJENA — main.cpp kliče graphStoreAdd() direktno
// graphNextSensor() je ODSTRANJENA — zamenjana z LVGL swipe event handler
// graphDrawLabel()  je ODSTRANJENA — ni bila v uporabi