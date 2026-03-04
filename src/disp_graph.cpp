// disp_graph.cpp - LVGL graf za vent_SEW (graph_update.md 2026-03-04)
//
// SPREMEMBE glede na prejšnjo verzijo:
//   - GraphStorePoint + gsHistory → GraphPoint + graphGetHistoryOrdered()
//   - GRAPH_MOTION UKINJEN (§5 spec)
//   - GRAPH_LUX (3) + GRAPH_CLOUD (5) DODANI
//   - FIX praznine na desni (§5.1): offset = DISPLAY_POINTS - validPts,
//     ser->y_points[offset+i] namesto obrnjenega polnjenja (ne lv_chart_set_value_by_id!)
//   - Moving average (§5.2): 3-točkovno drsno povprečje
//   - DISPLAY_POINTS = 240 (iz disp_graph.h), chart point count posodobljen
//   - GRAPH_WIND + GRAPH_CLOUD bereta iz GraphPoint (ne special weather veja)
//   - LVGL API: direkten dostop ser->y_points[] (lv_chart_set_value_by_id ne obstaja)

#include "disp_graph.h"
#include "graph_store.h"
#include "globals.h"
#include "logging.h"
#include <Preferences.h>
#include <time.h>
#include <math.h>

// -----------------------------------------------------------------------
// Minimalni razponi Y osi (preprečuje "drama" pri nizkih variacijah)
// Indeksi morajo ustrezati GraphSensor enum (GRAPH_COUNT = 6)
// -----------------------------------------------------------------------
static const float Y_MIN_RANGE[GRAPH_COUNT] = {
    3.0f,    // GRAPH_TEMP  (0) — min 3 °C razpon
    10.0f,   // GRAPH_HUM   (1) — min 10 %
    50.0f,   // GRAPH_IAQ   (2) — min 50 enot
    100.0f,  // GRAPH_LUX   (3) — min 100 lx
    5.0f,    // GRAPH_WIND  (4) — min 5 km/h
    20.0f,   // GRAPH_CLOUD (5) — min 20 %
};

// Skaliranje za LVGL (lv_coord_t je integer, 16-bit signed max 32767)
// Vrednosti pomnožimo s scale pred vpisom v ser->y_points[]
static int graphScale(int sensor) {
    switch (sensor) {
        case GRAPH_TEMP:  return 10;  // 21.3°C → 213
        case GRAPH_HUM:   return 10;  // 65.4%  → 654
        case GRAPH_IAQ:   return 1;   // 0-500  → direktno
        case GRAPH_LUX:   return 1;   // 0-xxxxx → direktno
        case GRAPH_WIND:  return 10;  // 12.5   → 125
        case GRAPH_CLOUD: return 1;   // 0-100  → direktno
        default:          return 10;
    }
}

// -----------------------------------------------------------------------
// NVS namespace za persistenco
// -----------------------------------------------------------------------
#define GRAPH_NVS_NS  "graph"

// -----------------------------------------------------------------------
// Globalne spremenljivke
// -----------------------------------------------------------------------
int currentGraphSensor = GRAPH_TEMP;

// -----------------------------------------------------------------------
// LVGL objekti (statični — vidni samo znotraj tega modula)
// -----------------------------------------------------------------------
static lv_obj_t*          graphCont  = nullptr;
static lv_obj_t*          chart      = nullptr;
static lv_chart_series_t* ser        = nullptr;
static lv_obj_t*          lbl_name   = nullptr;
static lv_obj_t*          lbl_minmax = nullptr;
static lv_obj_t*          lbl_ymax   = nullptr;
static lv_obj_t*          lbl_ymid   = nullptr;
static lv_obj_t*          lbl_ymin   = nullptr;
static lv_obj_t*          lbl_xL     = nullptr;
static lv_obj_t*          lbl_xM     = nullptr;
static lv_obj_t*          lbl_xR     = nullptr;

// -----------------------------------------------------------------------
// Touch detekcija
// -----------------------------------------------------------------------
static lv_point_t touchStart;
static uint32_t   touchStartMs = 0;

// -----------------------------------------------------------------------
// Sensor metapodatki
// -----------------------------------------------------------------------
const char* graphSensorName(int idx) {
    switch (idx) {
        case GRAPH_TEMP:  return "Temperatura";
        case GRAPH_HUM:   return "Vlaznost";
        case GRAPH_IAQ:   return "IAQ";
        case GRAPH_LUX:   return "Svetloba";
        case GRAPH_WIND:  return "Veter";
        case GRAPH_CLOUD: return "Oblacnost";
        default:          return "?";
    }
}

const char* graphSensorUnit(int idx) {
    switch (idx) {
        case GRAPH_TEMP:  return "\xC2\xB0""C";
        case GRAPH_HUM:   return "%";
        case GRAPH_IAQ:   return "";
        case GRAPH_LUX:   return "lx";
        case GRAPH_WIND:  return "km/h";
        case GRAPH_CLOUD: return "%";
        default:          return "";
    }
}

lv_color_t graphSensorColor(int idx) {
    switch (idx) {
        case GRAPH_TEMP:  return lv_color_hex(0xFF6B6B);   // topla rdeča
        case GRAPH_HUM:   return lv_color_hex(0x4DA6FF);   // modra
        case GRAPH_IAQ:   return lv_color_hex(0xFFD700);   // zlata
        case GRAPH_LUX:   return lv_color_hex(0xFFF176);   // rumena
        case GRAPH_WIND:  return lv_color_hex(0x80CBC4);   // teal
        case GRAPH_CLOUD: return lv_color_hex(0xB0BEC5);   // siva
        default:          return lv_color_hex(0xFFFFFF);
    }
}

// -----------------------------------------------------------------------
// calcYRange — dinamična Y os z minimalnim razponom (§5.5)
// Dela v naravnih enotah (pred skaliranjem).
// -----------------------------------------------------------------------
static void calcYRange(int sensor, float vMin, float vMax,
                       float& yMin, float& yMax) {
    float range    = vMax - vMin;
    float minRange = (sensor >= 0 && sensor < GRAPH_COUNT)
                     ? Y_MIN_RANGE[sensor] : 1.0f;

    if (range < minRange) {
        float center = (vMax + vMin) / 2.0f;
        yMin = center - minRange / 2.0f;
        yMax = center + minRange / 2.0f;
    } else {
        yMin = vMin - range * 0.10f;
        yMax = vMax + range * 0.10f;
    }
    yMin = floorf(yMin);
    yMax = ceilf(yMax);
}

// -----------------------------------------------------------------------
// NVS persistenca
// -----------------------------------------------------------------------
void loadGraphPrefs() {
    Preferences prefs;
    prefs.begin(GRAPH_NVS_NS, true);
    currentGraphSensor = prefs.getInt("sensor", GRAPH_TEMP);
    currentGraphHours  = prefs.getInt("hours",  4);
    prefs.end();

    if (currentGraphSensor < 0 || currentGraphSensor >= GRAPH_COUNT)
        currentGraphSensor = GRAPH_TEMP;
    const int validHours[] = {2, 4, 8, 16, 24};
    bool valid = false;
    for (int h : validHours) if (currentGraphHours == h) { valid = true; break; }
    if (!valid) currentGraphHours = 4;

    LOG_INFO("GRAPH", "Prefs loaded: sensor=%d hours=%d", currentGraphSensor, currentGraphHours);
}

void saveGraphPrefs() {
    Preferences prefs;
    prefs.begin(GRAPH_NVS_NS, false);
    prefs.putInt("sensor", currentGraphSensor);
    prefs.putInt("hours",  currentGraphHours);
    prefs.end();
}

// -----------------------------------------------------------------------
// Zoom (§5.4)
// -----------------------------------------------------------------------
static void graphZoomIn() {
    const int steps[] = {2, 4, 8, 16, 24};
    for (int i = 1; i < 5; i++) {
        if (currentGraphHours == steps[i]) {
            currentGraphHours = steps[i - 1];
            LOG_INFO("GRAPH", "Zoom IN: %dh", currentGraphHours);
            return;
        }
    }
}

static void graphZoomOut() {
    const int steps[] = {2, 4, 8, 16, 24};
    for (int i = 0; i < 4; i++) {
        if (currentGraphHours == steps[i]) {
            currentGraphHours = steps[i + 1];
            LOG_INFO("GRAPH", "Zoom OUT: %dh", currentGraphHours);
            return;
        }
    }
}

// -----------------------------------------------------------------------
// Touch event handler — swipe levo/desno = sensor, dotik = zoom (§5.8)
// -----------------------------------------------------------------------
static void graphTouchHandler(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        lv_indev_get_point(lv_indev_get_act(), &touchStart);
        touchStartMs = millis();
        return;
    }

    if (code == LV_EVENT_RELEASED) {
        lv_point_t touchEnd;
        lv_indev_get_point(lv_indev_get_act(), &touchEnd);
        uint32_t duration = millis() - touchStartMs;
        int16_t  dx       = touchEnd.x - touchStart.x;
        int16_t  dy       = touchEnd.y - touchStart.y;

        if (abs(dx) > 40 && abs(dx) > abs(dy)) {
            // SWIPE levo/desno — menja sensor
            if (dx < 0) {
                currentGraphSensor = (currentGraphSensor + 1) % GRAPH_COUNT;
                LOG_INFO("GRAPH", "Swipe left → sensor %d (%s)",
                         currentGraphSensor, graphSensorName(currentGraphSensor));
            } else {
                currentGraphSensor = (currentGraphSensor + GRAPH_COUNT - 1) % GRAPH_COUNT;
                LOG_INFO("GRAPH", "Swipe right → sensor %d (%s)",
                         currentGraphSensor, graphSensorName(currentGraphSensor));
            }
            if (chart && ser)
                lv_chart_set_series_color(chart, ser, graphSensorColor(currentGraphSensor));
        } else if (abs(dx) <= 40) {
            // TAP — zoom
            if (duration < 500) graphZoomIn();
            else                 graphZoomOut();
        }

        // Posodobi glavo
        if (lbl_name) {
            char buf[40];
            snprintf(buf, sizeof(buf), "%s  %dh",
                     graphSensorName(currentGraphSensor), currentGraphHours);
            lv_label_set_text(lbl_name, buf);
        }
        saveGraphPrefs();
        graphRefresh();
    }
}

// -----------------------------------------------------------------------
// initGraph
// -----------------------------------------------------------------------
void initGraph(lv_obj_t* parent, int x, int y, int w, int h) {
    // Preference se naložijo v main.cpp po graphLoadFromLittleFS()
    // (loadGraphPrefs() kliče main.cpp eksplicitno po init grafa)

    // Zunanji container
    graphCont = lv_obj_create(parent);
    lv_obj_set_pos(graphCont, x, y);
    lv_obj_set_size(graphCont, w, h);
    lv_obj_set_style_bg_color(graphCont, lv_color_hex(0x1A1A2A), 0);
    lv_obj_set_style_border_color(graphCont, lv_color_hex(0x333350), 0);
    lv_obj_set_style_border_width(graphCont, 1, 0);
    lv_obj_set_style_radius(graphCont, 8, 0);
    lv_obj_set_style_pad_all(graphCont, 4, 0);
    lv_obj_clear_flag(graphCont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(graphCont, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(graphCont, graphTouchHandler, LV_EVENT_PRESSED,  nullptr);
    lv_obj_add_event_cb(graphCont, graphTouchHandler, LV_EVENT_RELEASED, nullptr);

    // Glava: ime senzorja + ure (levo), min/max (desno)
    lbl_name = lv_label_create(graphCont);
    lv_obj_set_pos(lbl_name, 4, 4);
    lv_obj_set_style_text_color(lbl_name, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_name, &lv_font_montserrat_16, 0);
    {
        char buf[40];
        snprintf(buf, sizeof(buf), "%s  %dh",
                 graphSensorName(currentGraphSensor), currentGraphHours);
        lv_label_set_text(lbl_name, buf);
    }

    lbl_minmax = lv_label_create(graphCont);
    lv_obj_align(lbl_minmax, LV_ALIGN_TOP_RIGHT, -4, 4);
    lv_obj_set_style_text_font(lbl_minmax, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_minmax, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(lbl_minmax, "");

    // Chart dimenzije (prostor za Y oznake levo, X oznake spodaj)
    int chartX = 32, chartY = 24;
    int chartW = w - 40, chartH = h - 48;

    // Y osi oznake (desno poravnane, levo od chart-a)
    const lv_font_t* fAxis = &lv_font_montserrat_14;
    lv_color_t       cAxis = lv_color_hex(0xFFFFFF);

    auto makeAxisLbl = [&](int ax, int ay, int aw, const char* txt) -> lv_obj_t* {
        lv_obj_t* l = lv_label_create(graphCont);
        lv_obj_set_pos(l, ax, ay);
        lv_obj_set_width(l, aw);
        lv_obj_set_style_text_font(l, fAxis, 0);
        lv_obj_set_style_text_color(l, cAxis, 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_text(l, txt);
        return l;
    };

    lbl_ymax = makeAxisLbl(0, chartY,                    chartX - 2, "--");
    lbl_ymid = makeAxisLbl(0, chartY + chartH / 2 - 5,  chartX - 2, "--");
    lbl_ymin = makeAxisLbl(0, chartY + chartH - 10,      chartX - 2, "--");

    // X osi oznake (pod chart-om)
    int xAxisY = chartY + chartH + 2;

    lbl_xL = lv_label_create(graphCont);
    lv_obj_set_pos(lbl_xL, chartX, xAxisY);
    lv_obj_set_style_text_font(lbl_xL, fAxis, 0);
    lv_obj_set_style_text_color(lbl_xL, cAxis, 0);
    lv_label_set_text(lbl_xL, "--h");

    lbl_xM = lv_label_create(graphCont);
    lv_obj_set_pos(lbl_xM, chartX + chartW / 2 - 12, xAxisY);
    lv_obj_set_style_text_font(lbl_xM, fAxis, 0);
    lv_obj_set_style_text_color(lbl_xM, cAxis, 0);
    lv_label_set_text(lbl_xM, "--h");

    lbl_xR = lv_label_create(graphCont);
    lv_obj_set_pos(lbl_xR, chartX + chartW - 8, xAxisY);
    lv_obj_set_style_text_font(lbl_xR, fAxis, 0);
    lv_obj_set_style_text_color(lbl_xR, cAxis, 0);
    lv_label_set_text(lbl_xR, "0");

    // LVGL chart widget
    chart = lv_chart_create(graphCont);
    lv_obj_set_pos(chart, chartX, chartY);
    lv_obj_set_size(chart, chartW, chartH);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, DISPLAY_POINTS);   // 240 točk (1 px/točka pri 240px)
    lv_chart_set_div_line_count(chart, 3, 0);
    lv_obj_set_style_bg_color(chart, lv_color_hex(0x111120), 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_line_color(chart, lv_color_hex(0x2A2A40), 0);
    lv_obj_set_style_size(chart, 0, LV_PART_INDICATOR);

    ser = lv_chart_add_series(chart, graphSensorColor(currentGraphSensor), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(chart, ser, LV_CHART_POINT_NONE);

    // Touch propagira do graphCont
    lv_obj_add_flag(chart, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(chart, LV_OBJ_FLAG_EVENT_BUBBLE);

    LOG_INFO("GRAPH", "Graph initialized (%dx%d) DISPLAY_POINTS=%d", w, h, DISPLAY_POINTS);
}

// -----------------------------------------------------------------------
// graphRefresh — osveži graf iz graph_store (graph_update.md §5.3)
//
// Algoritem:
//   1. graphGetHistoryOrdered() → točke v kronološkem vrstnem redu
//   2. Filter po tStart (glede na currentGraphHours)
//   3. Moving average (3 točke, §5.2)
//   4. Fill from right (§5.1): praznina gre LEVO, najnovejša točka DESNO
//   5. Posodobi oznake osi
// -----------------------------------------------------------------------
void graphRefresh() {
    if (!chart || !ser) return;

    // --- 1. Pridobi točke iz ring bufferja v kronološkem vrstnem redu ---
    static GraphPoint histBuf[GRAPH_HISTORY_MAX];
    int total = graphGetHistoryOrdered(histBuf, GRAPH_HISTORY_MAX);

    // --- 2. Določi časovno okno ---
    uint32_t now    = (uint32_t)time(nullptr);
    uint32_t tStart = now - (uint32_t)(currentGraphHours * 3600UL);

    // --- 3. Filtriraj in izvleči vrednosti ---
    static float vals[DISPLAY_POINTS];
    int   validPts = 0;
    float vMin     =  1e9f;
    float vMax     = -1e9f;
    float lastVal  = -998.0f;

    const float NONE_SENTINEL = -998.0f;

    for (int i = 0; i < total && validPts < DISPLAY_POINTS; i++) {
        const GraphPoint& pt = histBuf[i];
        if (pt.ts < tStart) continue;  // starejše od časovnega okna — preskoči

        float v = NONE_SENTINEL;
        switch (currentGraphSensor) {
            case GRAPH_TEMP:  v = pt.temp;  break;
            case GRAPH_HUM:   v = pt.hum;   break;
            case GRAPH_IAQ:   v = pt.iaq;   break;
            case GRAPH_LUX:   v = pt.lux;   break;
            case GRAPH_WIND:  v = pt.wind;  break;
            case GRAPH_CLOUD: v = pt.cloud; break;
            default: break;
        }

        // Preveri veljavnost
        if (v <= NONE_SENTINEL) {
            vals[validPts++] = NONE_SENTINEL;
            continue;
        }

        if (v < vMin) vMin = v;
        if (v > vMax) vMax = v;
        lastVal = v;
        vals[validPts++] = v;
    }

    // --- 4. Ni dovolj podatkov ---
    if (validPts < 2) {
        lv_chart_set_all_value(chart, ser, LV_CHART_POINT_NONE);
        if (lbl_minmax) lv_label_set_text(lbl_minmax, "Ni podatkov");
        lv_chart_refresh(chart);
        return;
    }

    // --- 5. Moving average — 3-točkovno drsno povprečje (§5.2) ---
    static float smoothVals[DISPLAY_POINTS];
    smoothVals[0]             = vals[0];
    smoothVals[validPts - 1]  = vals[validPts - 1];

    for (int i = 1; i < validPts - 1; i++) {
        // Ne gladi čez praznine (NONE_SENTINEL vrednosti)
        if (vals[i - 1] <= NONE_SENTINEL || vals[i + 1] <= NONE_SENTINEL ||
            vals[i]     <= NONE_SENTINEL) {
            smoothVals[i] = vals[i];
        } else {
            smoothVals[i] = (vals[i - 1] + vals[i] + vals[i + 1]) / 3.0f;
        }
    }

    // --- 6. Izračunaj Y os ---
    float yMin, yMax;
    calcYRange(currentGraphSensor, vMin, vMax, yMin, yMax);

    int scale = graphScale(currentGraphSensor);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y,
                       (lv_coord_t)(yMin * scale),
                       (lv_coord_t)(yMax * scale));

    // --- 7. Fill from right (§5.1) — FIX vizualnega buga ---
    // Praznina (NONE) gre LEVO, najnovejša točka je skrajno DESNO.
    // Direkten dostop ser->y_points[] — lv_chart_set_value_by_id() ne obstaja!
    lv_chart_set_all_value(chart, ser, LV_CHART_POINT_NONE);
    int offset = DISPLAY_POINTS - validPts;  // praznina = offset točk na levi

    for (int i = 0; i < validPts; i++) {
        if (smoothVals[i] <= NONE_SENTINEL) {
            ser->y_points[offset + i] = LV_CHART_POINT_NONE;
        } else {
            ser->y_points[offset + i] = (lv_coord_t)(smoothVals[i] * scale);
        }
    }
    lv_chart_refresh(chart);

    // --- 8. Posodobi serijo barvo ---
    lv_chart_set_series_color(chart, ser, graphSensorColor(currentGraphSensor));

    // --- 9. Posodobi glavo: min/max ---
    char bufMM[32];
    const char* unit = graphSensorUnit(currentGraphSensor);
    snprintf(bufMM, sizeof(bufMM), "%.1f / %.1f %s", vMin, vMax, unit);
    if (lbl_minmax) lv_label_set_text(lbl_minmax, bufMM);

    if (lbl_name) {
        char buf[40];
        snprintf(buf, sizeof(buf), "%s  %dh",
                 graphSensorName(currentGraphSensor), currentGraphHours);
        lv_label_set_text(lbl_name, buf);
    }

    // --- 10. Oznake Y osi (min / mid / max) ---
    char bufY[12];
    float yMidVal = (yMin + yMax) / 2.0f;
    snprintf(bufY, sizeof(bufY), "%.0f", yMax);    if (lbl_ymax) lv_label_set_text(lbl_ymax, bufY);
    snprintf(bufY, sizeof(bufY), "%.0f", yMidVal); if (lbl_ymid) lv_label_set_text(lbl_ymid, bufY);
    snprintf(bufY, sizeof(bufY), "%.0f", yMin);    if (lbl_ymin) lv_label_set_text(lbl_ymin, bufY);

    // --- 11. Oznake X osi (relativni čas) ---
    char xbuf[8];
    snprintf(xbuf, sizeof(xbuf), "-%dh", currentGraphHours);
    if (lbl_xL) lv_label_set_text(lbl_xL, xbuf);
    snprintf(xbuf, sizeof(xbuf), "-%dh", currentGraphHours / 2);
    if (lbl_xM) lv_label_set_text(lbl_xM, xbuf);
    if (lbl_xR) lv_label_set_text(lbl_xR, "0");

    LOG_DEBUG("GRAPH", "Refresh: sensor=%d pts=%d/%d yMin=%.1f yMax=%.1f",
              currentGraphSensor, validPts, total, yMin, yMax);
}
