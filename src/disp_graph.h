// disp_graph.h - Display graph module for vent_SEW (LVGL)
// Graf zavzema spodnjo polovico glavnega zaslona (~190px od 320px)
// Klik na graf -> menja prikazano vrednost
#pragma once
#include <Arduino.h>
#include <lvgl.h>

// Tipi grafov: lokalni senzorji + OpenMeteo
enum GraphSensor {
    GRAPH_TEMP = 0,         // Temperatura [C]    - SHT4x
    GRAPH_HUM,              // Vlaznost [%]        - SHT4x
    GRAPH_PRESS,            // Tlak [hPa]          - BSEC
    GRAPH_IAQ,              // IAQ [0-500]         - BSEC
    GRAPH_CO2,              // eCO2 [ppm]          - BSEC
    GRAPH_LUX,              // Svetloba [lux]      - TCS34725
    // OpenMeteo
    GRAPH_WIND,             // Hitrost vetra [km/h]
    GRAPH_CLOUD,            // Pokritost oblakov [%]
    GRAPH_SOIL_TEMP,        // Temperatura tal [C]
    GRAPH_SOIL_MOIST,       // Vlaznost tal [%]
    GRAPH_COUNT
};

// Zgodovinski zapis za lokalne senzorje
struct GraphPoint {
    unsigned long ts;
    float temp;
    float hum;
    float press;
    float iaq;
    float co2;
    float lux;
};

// 24h pri 30s intervalu = 2880 tock
#define GRAPH_HISTORY_MAX  2880

extern int currentGraphSensor;

void initGraph(lv_obj_t* parent, int x, int y, int w, int h);
void graphAddPoint(const GraphPoint& pt);
void graphNextSensor();
void graphRefresh();
void graphDrawLabel(lv_obj_t* parent);

const char* graphSensorName(int idx);
const char* graphSensorUnit(int idx);
lv_color_t  graphSensorColor(int idx);
