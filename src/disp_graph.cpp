// disp_graph.cpp - Display graph module for vent_SEW (LVGL)
//
// POPRAVKI (2026-03-02):
//   - FIX: ser->y_points[] direkten dostop ODSTRANJEN.
//     lv_chart_series_t je interni LVGL tip - direkten dostop na y_points[]
//     zaobide LVGL cache mehanizem → graf se ne osveži pravilno.
//     NAPAČNO (prej):
//       for (int i = 0; i < validPts; i++) ser->y_points[i] = vals[i];
//       lv_chart_refresh(chart);
//     PRAVILNO (zdaj):
//       lv_chart_set_all_value(chart, ser, LV_CHART_POINT_NONE);  // reset
//       for (int i = 0; i < validPts; i++)
//           lv_chart_set_value_by_id(chart, ser, i, vals[i]);     // API klic
//       lv_chart_refresh(chart);
//     lv_chart_set_value_by_id() je LVGL8 API ki pravilno invalidira cache.

#include "disp_graph.h"
#include "globals.h"
#include "weather.h"
#include "logging.h"
#include "colours.h"
#include <string.h>

int currentGraphSensor = GRAPH_TEMP;

static GraphPoint history[GRAPH_HISTORY_MAX];
static int historyCount = 0;
static int historyHead  = 0;

static lv_obj_t* chart        = nullptr;
static lv_chart_series_t* ser = nullptr;
static lv_obj_t* lbl_name_unit = nullptr; // združeno ime + enota
static lv_obj_t* lbl_minmax   = nullptr;

#define DISPLAY_POINTS  120   // ~1h pri 30s intervalu

const char* graphSensorName(int idx) {
    switch (idx) {
        case GRAPH_TEMP:       return "Temperatura";
        case GRAPH_HUM:        return "Vlaznost";
        case GRAPH_PRESS:      return "Tlak";
        case GRAPH_IAQ:        return "IAQ";
        case GRAPH_CO2:        return "eCO2";
        case GRAPH_LUX:        return "Svetloba";
        case GRAPH_WIND:       return "Veter";
        case GRAPH_CLOUD:      return "Oblacnost";
        case GRAPH_SOIL_TEMP:  return "Temp. tal";
        case GRAPH_SOIL_MOIST: return "Vlaznost tal";
        default:               return "?";
    }
}

const char* graphSensorUnit(int idx) {
    switch (idx) {
        case GRAPH_TEMP:       return "\xC2\xB0""C";
        case GRAPH_HUM:        return "%";
        case GRAPH_PRESS:      return "hPa";
        case GRAPH_IAQ:        return "";
        case GRAPH_CO2:        return "ppm";
        case GRAPH_LUX:        return "lux";
        case GRAPH_WIND:       return "km/h";
        case GRAPH_CLOUD:      return "%";
        case GRAPH_SOIL_TEMP:  return "\xC2\xB0""C";
        case GRAPH_SOIL_MOIST: return "%";
        default:               return "";
    }
}

lv_color_t graphSensorColor(int idx) {
    switch (idx) {
        case GRAPH_TEMP:       return lv_color_hex(0xFF6B6B);
        case GRAPH_HUM:        return lv_color_hex(0x4DA6FF);
        case GRAPH_PRESS:      return lv_color_hex(0xA8D8A8);
        case GRAPH_IAQ:        return lv_color_hex(0xFFD700);
        case GRAPH_CO2:        return lv_color_hex(0xFF8C42);
        case GRAPH_LUX:        return lv_color_hex(0xFFF176);
        case GRAPH_WIND:       return lv_color_hex(0x80CBC4);
        case GRAPH_CLOUD:      return lv_color_hex(0xB0BEC5);
        case GRAPH_SOIL_TEMP:  return lv_color_hex(0xA1887F);
        case GRAPH_SOIL_MOIST: return lv_color_hex(0x64B5F6);
        default:               return lv_color_hex(0xFFFFFF);
    }
}

static float getHistoryValue(int logIdx, int sensor) {
    int physIdx = (historyHead + logIdx) % GRAPH_HISTORY_MAX;
    const GraphPoint& p = history[physIdx];
    switch (sensor) {
        case GRAPH_TEMP:  return p.temp;
        case GRAPH_HUM:   return p.hum;
        case GRAPH_PRESS: return p.press;
        case GRAPH_IAQ:   return p.iaq;
        case GRAPH_CO2:   return p.co2;
        case GRAPH_LUX:   return p.lux;
        default:          return 0.0f;
    }
}

static float getWeatherValue(int sensor) {
    if (!weatherData.valid) return 0.0f;
    switch (sensor) {
        case GRAPH_WIND:       return weatherData.windSpeed;
        case GRAPH_CLOUD:      return (float)weatherData.cloudCover;
        case GRAPH_SOIL_TEMP:  return weatherData.soilTemp;
        case GRAPH_SOIL_MOIST: return weatherData.soilMoisture * 100.0f;
        default:               return 0.0f;
    }
}

void initGraph(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* cont = lv_obj_create(parent);
    lv_obj_set_pos(cont, x, y);
    lv_obj_set_size(cont, w, h);
    lv_obj_set_style_bg_color(cont, lv_color_hex(0x1A1A2A), 0);
    lv_obj_set_style_border_color(cont, lv_color_hex(0x333350), 0);
    lv_obj_set_style_border_width(cont, 1, 0);
    lv_obj_set_style_radius(cont, 8, 0);
    lv_obj_set_style_pad_all(cont, 4, 0);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    // Združen label za ime senzorja in enoto (levo zgoraj)
    lbl_name_unit = lv_label_create(cont);
    lv_obj_set_pos(lbl_name_unit, 4, 4);
    lv_obj_set_style_text_color(lbl_name_unit, lv_color_hex(0xFFFFFF), 0); // popolnoma bela kot ura
    lv_obj_set_style_text_font(lbl_name_unit, &lv_font_montserrat_16, 0);
    
    // Nastavi začetno vrednost
    char name_unit_init[32];
    const char* name_init = graphSensorName(currentGraphSensor);
    const char* unit_init = graphSensorUnit(currentGraphSensor);
    if (unit_init[0] != '\0') {
        snprintf(name_unit_init, sizeof(name_unit_init), "%s %s", name_init, unit_init);
    } else {
        snprintf(name_unit_init, sizeof(name_unit_init), "%s", name_init);
    }
    lv_label_set_text(lbl_name_unit, name_unit_init);

    // Label za min/max (desno zgoraj)
    lbl_minmax = lv_label_create(cont);
    lv_obj_set_pos(lbl_minmax, w - 8, 4); // desna pozicija, poravnava desno
    lv_obj_set_width(lbl_minmax, 80); // dovolj prostora za "XX.X / XX.X"
    lv_obj_set_style_text_align(lbl_minmax, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(lbl_minmax, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_minmax, &lv_font_montserrat_16, 0);
    lv_label_set_text(lbl_minmax, "");

    // Graf začnemo nekoliko višje, ker imamo samo eno vrstico napisov
    chart = lv_chart_create(cont);
    lv_obj_set_pos(chart, 0, 22);
    lv_obj_set_size(chart, w - 8, h - 30);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, DISPLAY_POINTS);
    lv_chart_set_div_line_count(chart, 3, 0);
    lv_obj_set_style_bg_color(chart, lv_color_hex(0x111120), 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_line_color(chart, lv_color_hex(0x2A2A40), 0);
    lv_obj_set_style_size(chart, 0, LV_PART_INDICATOR);

    ser = lv_chart_add_series(chart, graphSensorColor(currentGraphSensor), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(chart, ser, 0);

    LOG_INFO("GRAPH", "Graph initialized (%dx%d) at (%d,%d)", w, h, x, y);
}

void graphAddPoint(const GraphPoint& pt) {
    if (historyCount < GRAPH_HISTORY_MAX) {
        history[(historyHead + historyCount) % GRAPH_HISTORY_MAX] = pt;
        historyCount++;
    } else {
        history[historyHead] = pt;
        historyHead = (historyHead + 1) % GRAPH_HISTORY_MAX;
    }
}

void graphNextSensor() {
    currentGraphSensor = (currentGraphSensor + 1) % GRAPH_COUNT;
    LOG_INFO("GRAPH", "Switched to sensor %d (%s)", currentGraphSensor, graphSensorName(currentGraphSensor));
    if (!chart || !ser) return;
    lv_chart_set_series_color(chart, ser, graphSensorColor(currentGraphSensor));
    
    // Posodobi združen label za ime in enoto
    char name_unit[32];
    const char* name = graphSensorName(currentGraphSensor);
    const char* unit = graphSensorUnit(currentGraphSensor);
    if (unit[0] != '\0') {
        snprintf(name_unit, sizeof(name_unit), "%s %s", name, unit);
    } else {
        snprintf(name_unit, sizeof(name_unit), "%s", name);
    }
    lv_label_set_text(lbl_name_unit, name_unit);
    
    graphRefresh();
}

void graphRefresh() {
    if (!chart || !ser) return;

    bool isWeatherSensor = (currentGraphSensor >= GRAPH_WIND);

    if (isWeatherSensor) {
        float val = getWeatherValue(currentGraphSensor);
        lv_chart_set_all_value(chart, ser, (lv_coord_t)(val * 10));
        lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, (lv_coord_t)(val * 10 * 2 + 10));
        
        // Nastavi ime + enoto (če še ni nastavljeno)
        char name_unit[32];
        const char* name = graphSensorName(currentGraphSensor);
        const char* unit = graphSensorUnit(currentGraphSensor);
        if (unit[0] != '\0') {
            snprintf(name_unit, sizeof(name_unit), "%s %s", name, unit);
        } else {
            snprintf(name_unit, sizeof(name_unit), "%s", name);
        }
        lv_label_set_text(lbl_name_unit, name_unit);
        
        lv_label_set_text(lbl_minmax, "(OpenMeteo)");
        lv_chart_refresh(chart);
        return;
    }

    int pts = min(historyCount, DISPLAY_POINTS);
    if (pts < 2) {
        lv_chart_set_all_value(chart, ser, LV_CHART_POINT_NONE);
        lv_label_set_text(lbl_minmax, "Ni podatkov");
        lv_chart_refresh(chart);
        return;
    }

    int startLog = max(0, historyCount - DISPLAY_POINTS);
    float vMin =  1e9f, vMax = -1e9f, lastVal = 0.0f;
    for (int i = startLog; i < historyCount; i++) {
        float v = getHistoryValue(i, currentGraphSensor);
        if (v <= -998.0f) continue;
        if (v < vMin) vMin = v;
        if (v > vMax) vMax = v;
        lastVal = v;
    }

    if (vMin > vMax) {
        lv_chart_set_all_value(chart, ser, LV_CHART_POINT_NONE);
        lv_label_set_text(lbl_minmax, "--/--");
        lv_chart_refresh(chart);
        return;
    }

    float range = vMax - vMin;
    if (range < 1.0f) range = 1.0f;
    int scale = 10;
    if (currentGraphSensor == GRAPH_PRESS || currentGraphSensor == GRAPH_CO2 || currentGraphSensor == GRAPH_LUX)
        scale = 1;

    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y,
        (lv_coord_t)((vMin - range * 0.1f) * scale),
        (lv_coord_t)((vMax + range * 0.1f) * scale));

    // Pripravi vrednosti
    lv_coord_t vals[DISPLAY_POINTS];
    int validPts = 0;
    for (int i = startLog; i < historyCount && validPts < DISPLAY_POINTS; i++) {
        float v = getHistoryValue(i, currentGraphSensor);
        vals[validPts++] = (v <= -998.0f) ? LV_CHART_POINT_NONE : (lv_coord_t)(v * scale);
    }

    // FIX (2026-03-02): ser->y_points[] direkten dostop ZAMENJAN z LVGL8 API.
    // Direkten dostop zaobide LVGL interni cache → lv_chart_refresh() nima
    // informacije katere točke so se spremenile → graf se ne izriše pravilno.
    //
    // NAPAČNO (bilo prej):
    //   lv_chart_set_all_value(chart, ser, LV_CHART_POINT_NONE);
    //   for (int i = 0; i < validPts; i++) ser->y_points[i] = vals[i];  // ← direkten dostop!
    //   lv_chart_refresh(chart);
    //
    // PRAVILNO (zdaj):
    //   lv_chart_set_all_value()    → reset vseh točk (pravilno invalidira cache)
    //   lv_chart_set_value_by_id()  → nastavi vsako točko prek LVGL API
    //   lv_chart_refresh()          → LVGL ve točno katere regije prerisati
    lv_chart_set_all_value(chart, ser, LV_CHART_POINT_NONE);
    for (int i = 0; i < validPts; i++) {
        lv_chart_set_value_by_id(chart, ser, i, vals[i]);
    }
    lv_chart_refresh(chart);

    // Nastavi ime + enoto (če še ni nastavljeno)
    char name_unit[32];
    const char* name = graphSensorName(currentGraphSensor);
    const char* unit = graphSensorUnit(currentGraphSensor);
    if (unit[0] != '\0') {
        snprintf(name_unit, sizeof(name_unit), "%s %s", name, unit);
    } else {
        snprintf(name_unit, sizeof(name_unit), "%s", name);
    }
    lv_label_set_text(lbl_name_unit, name_unit);

    // Prikaži min/max
    char bufMM[32];
    if (currentGraphSensor == GRAPH_PRESS || currentGraphSensor == GRAPH_CO2) {
        snprintf(bufMM, sizeof(bufMM), "%.0f / %.0f", vMin, vMax);
    } else if (currentGraphSensor == GRAPH_LUX) {
        snprintf(bufMM, sizeof(bufMM), "%.0f / %.0f", vMin, vMax);
    } else {
        snprintf(bufMM, sizeof(bufMM), "%.1f / %.1f", vMin, vMax);
    }
    lv_label_set_text(lbl_minmax, bufMM);
}

void graphDrawLabel(lv_obj_t* parent) { (void)parent; }
