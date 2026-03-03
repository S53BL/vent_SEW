// disp_graph.cpp - LVGL graf za vent_SEW
//
// POPRAVKI (2026-03-03, korak 3):
//   - Lokalni history[] RAM buffer ODSTRANJEN — podatki so v graph_store.cpp
//   - graphRefresh() bere iz gsHistory[] prek graphStoreGet()
//   - Časovno okno (currentGraphHours) filtrira točke po timestamp
//   - Swipe levo/desno: menja sensor; kratek dotik: zoom in; dolgi: zoom out
//   - calcYRange(): dinamična Y os z minimalnim razponom (preprečuje "drama")
//   - NVS persistenca: saveGraphPrefs() / loadGraphPrefs()
//   - LVGL8 API: lv_chart_set_value_by_id() — brez direktnega dostopa na y_points

#include "disp_graph.h"
#include "graph_store.h"
#include "globals.h"
#include "weather.h"
#include "logging.h"
#include <Preferences.h>
#include <time.h>
#include <math.h>

// -----------------------------------------------------------------------
// Konstante
// -----------------------------------------------------------------------

// Maksimalno število LVGL točk na grafu (pri 3-min intervalu in 24h oknu = 480)
#define DISPLAY_POINTS_MAX  480

// Minimalni razponi Y osi po senzorju (preprečuje "drama" pri majhnih variacijah)
static const float Y_MIN_RANGE[GRAPH_COUNT] = {
    3.0f,    // GRAPH_TEMP    — min razpon 3 °C
    10.0f,   // GRAPH_HUM     — min razpon 10 %
    50.0f,   // GRAPH_IAQ     — min razpon 50 enot
    1.0f,    // GRAPH_MOTION  — ni linijski, posebna logika
    5.0f     // GRAPH_WIND    — min razpon 5 km/h
};

// NVS namespace za persistenco prefs
#define GRAPH_NVS_NS  "graph"

// -----------------------------------------------------------------------
// Globalne spremenljivke
// -----------------------------------------------------------------------
int currentGraphSensor = GRAPH_TEMP;

// -----------------------------------------------------------------------
// LVGL objekti (statični — vidni samo znotraj tega modula)
// -----------------------------------------------------------------------
static lv_obj_t*          graphCont    = nullptr;  // zunanji container
static lv_obj_t*          chart        = nullptr;
static lv_chart_series_t* ser          = nullptr;
static lv_obj_t*          lbl_name     = nullptr;
static lv_obj_t*          lbl_unit     = nullptr;
static lv_obj_t*          lbl_current  = nullptr;
static lv_obj_t*          lbl_minmax   = nullptr;
static lv_obj_t*          lbl_hours    = nullptr;  // prikaz "4h", "8h" itd.

// -----------------------------------------------------------------------
// Touch detekcija (swipe / kratek / dolgi dotik)
// -----------------------------------------------------------------------
static lv_point_t touchStart;
static uint32_t   touchStartMs = 0;

// -----------------------------------------------------------------------
// Pomožne funkcije — ime / enota / barva
// -----------------------------------------------------------------------

const char* graphSensorName(int idx) {
    switch (idx) {
        case GRAPH_TEMP:   return "Temperatura";
        case GRAPH_HUM:    return "Vlaznost";
        case GRAPH_IAQ:    return "IAQ";
        case GRAPH_MOTION: return "Gibanje";
        case GRAPH_WIND:   return "Veter";
        default:           return "?";
    }
}

const char* graphSensorUnit(int idx) {
    switch (idx) {
        case GRAPH_TEMP:   return "\xC2\xB0""C";
        case GRAPH_HUM:    return "%";
        case GRAPH_IAQ:    return "";
        case GRAPH_MOTION: return "ev/h";
        case GRAPH_WIND:   return "km/h";
        default:           return "";
    }
}

lv_color_t graphSensorColor(int idx) {
    switch (idx) {
        case GRAPH_TEMP:   return lv_color_hex(0xFF6B6B);
        case GRAPH_HUM:    return lv_color_hex(0x4DA6FF);
        case GRAPH_IAQ:    return lv_color_hex(0xFFD700);
        case GRAPH_MOTION: return lv_color_hex(0xA8D8A8);
        case GRAPH_WIND:   return lv_color_hex(0x80CBC4);
        default:           return lv_color_hex(0xFFFFFF);
    }
}

// -----------------------------------------------------------------------
// calcYRange — dinamična Y os z minimalnim razponom
// -----------------------------------------------------------------------
static void calcYRange(int sensor, float vMin, float vMax,
                       float& yMin, float& yMax) {
    float range = vMax - vMin;
    float minRange = Y_MIN_RANGE[sensor];

    if (range < minRange) {
        float center = (vMax + vMin) / 2.0f;
        yMin = center - minRange / 2.0f;
        yMax = center + minRange / 2.0f;
    } else {
        yMin = vMin - range * 0.10f;
        yMax = vMax + range * 0.10f;
    }
    // Zaokroži na celo število
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

    // Validacija
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
// Zoom in / out
// -----------------------------------------------------------------------
static void graphZoomIn() {
    // 24 → 16 → 8 → 4 → 2
    const int steps[] = {2, 4, 8, 16, 24};
    for (int i = 1; i < 5; i++) {
        if (currentGraphHours == steps[i]) {
            currentGraphHours = steps[i - 1];
            LOG_INFO("GRAPH", "Zoom IN: %dh", currentGraphHours);
            return;
        }
    }
    // Že na minimumu (2h) — ne stori nič
}

static void graphZoomOut() {
    // 2 → 4 → 8 → 16 → 24
    const int steps[] = {2, 4, 8, 16, 24};
    for (int i = 0; i < 4; i++) {
        if (currentGraphHours == steps[i]) {
            currentGraphHours = steps[i + 1];
            LOG_INFO("GRAPH", "Zoom OUT: %dh", currentGraphHours);
            return;
        }
    }
    // Že na maksimumu (24h) — ne stori nič
}

// -----------------------------------------------------------------------
// LVGL event handler — swipe + dotik
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
        int16_t  dx = touchEnd.x - touchStart.x;

        if (abs(dx) > 40) {
            // SWIPE — menja sensor
            if (dx < 0) {
                // swipe levo → naslednji sensor
                currentGraphSensor = (currentGraphSensor + 1) % GRAPH_COUNT;
                LOG_INFO("GRAPH", "Swipe left → sensor %d (%s)",
                         currentGraphSensor, graphSensorName(currentGraphSensor));
            } else {
                // swipe desno → prejšnji sensor
                currentGraphSensor = (currentGraphSensor + GRAPH_COUNT - 1) % GRAPH_COUNT;
                LOG_INFO("GRAPH", "Swipe right → sensor %d (%s)",
                         currentGraphSensor, graphSensorName(currentGraphSensor));
            }
            // Posodobi barvo serije in labele
            if (chart && ser) {
                lv_chart_set_series_color(chart, ser, graphSensorColor(currentGraphSensor));
            }
            if (lbl_name) lv_label_set_text(lbl_name, graphSensorName(currentGraphSensor));
            if (lbl_unit) lv_label_set_text(lbl_unit, graphSensorUnit(currentGraphSensor));
            if (lbl_current) lv_obj_set_style_text_color(lbl_current, graphSensorColor(currentGraphSensor), 0);

        } else if (duration < 500) {
            // KRATEK DOTIK — zoom in (krajše časovno okno)
            graphZoomIn();
        } else {
            // DOLGI DOTIK — zoom out (daljše časovno okno)
            graphZoomOut();
        }

        // Posodobi prikaz ur in osveži graf
        if (lbl_hours) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%dh", currentGraphHours);
            lv_label_set_text(lbl_hours, buf);
        }
        saveGraphPrefs();
        graphRefresh();
    }
}

// -----------------------------------------------------------------------
// initGraph
// -----------------------------------------------------------------------
void initGraph(lv_obj_t* parent, int x, int y, int w, int h) {
    // Naloži shranjene preference
    loadGraphPrefs();

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

    // Registriraj touch event handler na container
    lv_obj_add_flag(graphCont, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(graphCont, graphTouchHandler, LV_EVENT_PRESSED,   nullptr);
    lv_obj_add_event_cb(graphCont, graphTouchHandler, LV_EVENT_RELEASED,  nullptr);

    // Labela: ime senzorja (levo zgoraj)
    lbl_name = lv_label_create(graphCont);
    lv_obj_set_pos(lbl_name, 4, 2);
    lv_obj_set_style_text_color(lbl_name, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(lbl_name, &lv_font_montserrat_12, 0);
    lv_label_set_text(lbl_name, graphSensorName(currentGraphSensor));

    // Labela: enota (levo, pod imenom)
    lbl_unit = lv_label_create(graphCont);
    lv_obj_set_pos(lbl_unit, 4, 16);
    lv_obj_set_style_text_font(lbl_unit, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(lbl_unit, lv_color_hex(0x888888), 0);
    lv_label_set_text(lbl_unit, graphSensorUnit(currentGraphSensor));

    // Labela: trenutna vrednost (desno zgoraj)
    lbl_current = lv_label_create(graphCont);
    lv_obj_align(lbl_current, LV_ALIGN_TOP_RIGHT, -4, 2);
    lv_obj_set_style_text_font(lbl_current, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_current, graphSensorColor(currentGraphSensor), 0);
    lv_label_set_text(lbl_current, "--");

    // Labela: min/max ali časovno okno (desno, pod current)
    lbl_minmax = lv_label_create(graphCont);
    lv_obj_align(lbl_minmax, LV_ALIGN_TOP_RIGHT, -4, 20);
    lv_obj_set_style_text_font(lbl_minmax, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(lbl_minmax, lv_color_hex(0x666688), 0);
    lv_label_set_text(lbl_minmax, "");

    // Labela: časovno okno (levo spodaj od naslova, npr. "4h")
    lbl_hours = lv_label_create(graphCont);
    lv_obj_set_pos(lbl_hours, 4, 28);
    lv_obj_set_style_text_font(lbl_hours, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(lbl_hours, lv_color_hex(0x555577), 0);
    {
        char buf[8];
        snprintf(buf, sizeof(buf), "%dh", currentGraphHours);
        lv_label_set_text(lbl_hours, buf);
    }

    // LVGL chart widget
    chart = lv_chart_create(graphCont);
    lv_obj_set_pos(chart, 0, 40);
    lv_obj_set_size(chart, w - 8, h - 48);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, DISPLAY_POINTS_MAX);
    lv_chart_set_div_line_count(chart, 3, 0);
    lv_obj_set_style_bg_color(chart, lv_color_hex(0x111120), 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_line_color(chart, lv_color_hex(0x2A2A40), 0);
    lv_obj_set_style_size(chart, 0, LV_PART_INDICATOR);

    ser = lv_chart_add_series(chart, graphSensorColor(currentGraphSensor), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(chart, ser, LV_CHART_POINT_NONE);

    LOG_INFO("GRAPH", "Graph initialized (%dx%d) at (%d,%d) sensor=%d hours=%d",
             w, h, x, y, currentGraphSensor, currentGraphHours);
}

// -----------------------------------------------------------------------
// graphRefresh — osveži graf iz graph_store glede na časovno okno
// -----------------------------------------------------------------------
void graphRefresh() {
    if (!chart || !ser) return;

    // --- 1. Določi časovno okno ---
    uint32_t now    = (uint32_t)time(nullptr);
    uint32_t tStart = now - (uint32_t)(currentGraphHours * 3600UL);

    // --- 2. Poseben primer: GRAPH_WIND — samo trenutna vrednost iz OpenMeteo ---
    if (currentGraphSensor == GRAPH_WIND) {
        float val = weatherData.valid ? weatherData.windSpeed : 0.0f;
        if (!weatherData.valid) {
            lv_chart_set_all_value(chart, ser, LV_CHART_POINT_NONE);
            lv_label_set_text(lbl_current, "--");
            lv_label_set_text(lbl_minmax, "Ni podatkov");
            lv_chart_refresh(chart);
            return;
        }
        // Prikaži trend iz graph_store (wind polje)
        // Pade skozi v splošno logiko spodaj — wind je shranjen v GraphStorePoint.wind
    }

    // --- 3. Filtriraj točke iz ring bufferja glede na tStart ---
    // Zberemo indekse točk ki so v časovnem oknu
    static lv_coord_t vals[DISPLAY_POINTS_MAX];
    int    validPts = 0;
    float  vMin     =  1e9f;
    float  vMax     = -1e9f;
    float  lastVal  = -998.0f;

    // Štetje gibanja po urah (za GRAPH_MOTION stolpčni graf)
    // Razdelimo currentGraphHours ur v buckete po 1 uri
    int motionBuckets[24] = {0};  // max 24 ur
    int numBuckets = currentGraphHours;  // 1 bucket = 1 ura

    for (int i = 0; i < gsCount; i++) {
        const GraphStorePoint* pt = graphStoreGet(i);
        if (!pt) continue;
        if (pt->ts < tStart) continue;  // izven okna — preskoči

        if (currentGraphSensor == GRAPH_MOTION) {
            // Razvrsti v urni bucket
            int ageSeconds = (int)(now - pt->ts);
            int bucketIdx  = ageSeconds / 3600;  // 0 = zadnja ura, N = starejše
            if (bucketIdx >= 0 && bucketIdx < numBuckets) {
                motionBuckets[bucketIdx] += pt->motion;
            }
            continue;
        }

        // Izvleci vrednost za trenutni senzor
        float v = -998.0f;
        switch (currentGraphSensor) {
            case GRAPH_TEMP: v = pt->temp;         break;
            case GRAPH_HUM:  v = pt->hum;          break;
            case GRAPH_IAQ:  v = pt->iaq;          break;
            case GRAPH_WIND: v = pt->wind;         break;
            default:         v = -998.0f;        break;
        }

        if (v <= -998.0f) {
            if (validPts < DISPLAY_POINTS_MAX) vals[validPts++] = LV_CHART_POINT_NONE;
            continue;
        }

        if (v < vMin) vMin = v;
        if (v > vMax) vMax = v;
        lastVal = v;

        if (validPts < DISPLAY_POINTS_MAX) {
            vals[validPts++] = (lv_coord_t)(v * 10.0f);
        }
    }

    // --- 4. Posebna obravnava GRAPH_MOTION (stolpčni) ---
    if (currentGraphSensor == GRAPH_MOTION) {
        // Stolpčni graf: Y od 0 navzgor, en stolpec = 1 ura
        int maxEvents = 0;
        for (int i = 0; i < numBuckets; i++)
            if (motionBuckets[i] > maxEvents) maxEvents = motionBuckets[i];

        int yMax = max(5, ((maxEvents / 5) + 1) * 5);  // zaokroži navzgor na 5

        lv_chart_set_type(chart, LV_CHART_TYPE_BAR);
        lv_chart_set_point_count(chart, numBuckets);
        lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, yMax);
        lv_chart_set_all_value(chart, ser, 0);

        // Zapiši vrednosti — bucket 0 je najnovejši, prikazujemo od starega k novemu
        for (int i = 0; i < numBuckets; i++) {
            int bucketFromEnd = numBuckets - 1 - i;  // obrni: levo=staro, desno=novo
            lv_chart_set_value_by_id(chart, ser, i, motionBuckets[bucketFromEnd]);
        }
        lv_chart_refresh(chart);

        char bufCur[16];
        snprintf(bufCur, sizeof(bufCur), "%d ev", motionBuckets[0]);
        lv_label_set_text(lbl_current, bufCur);
        snprintf(bufCur, sizeof(bufCur), "max %d/h", maxEvents);
        lv_label_set_text(lbl_minmax, bufCur);
        return;
    }

    // --- 5. Premakni nazaj na linijski graf (če smo bili na stolpčnem) ---
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, DISPLAY_POINTS_MAX);

    // --- 6. Preveri ali imamo dovolj podatkov ---
    if (validPts < 2) {
        lv_chart_set_all_value(chart, ser, LV_CHART_POINT_NONE);
        if (lbl_current) lv_label_set_text(lbl_current, "--");
        if (lbl_minmax)  lv_label_set_text(lbl_minmax, "Ni podatkov");
        lv_chart_refresh(chart);
        return;
    }

    // --- 7. Izračunaj Y os z minimalnim razponom ---
    float yMin, yMax;
    calcYRange(currentGraphSensor, vMin, vMax, yMin, yMax);

    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y,
                       (lv_coord_t)(yMin * 10.0f),
                       (lv_coord_t)(yMax * 10.0f));

    // --- 8. Zapiši točke v LVGL (LVGL8 API — ne direktno y_points!) ---
    lv_chart_set_all_value(chart, ser, LV_CHART_POINT_NONE);
    for (int i = 0; i < validPts; i++) {
        lv_chart_set_value_by_id(chart, ser, i, vals[i]);
    }
    lv_chart_refresh(chart);

    // --- 9. Posodobi labele ---
    char bufCur[24], bufMM[32];

    if (lastVal > -998.0f) {
        snprintf(bufCur, sizeof(bufCur), "%.1f %s", lastVal, graphSensorUnit(currentGraphSensor));
    } else {
        snprintf(bufCur, sizeof(bufCur), "--");
    }
    lv_label_set_text(lbl_current, bufCur);

    snprintf(bufMM, sizeof(bufMM), "%.1f / %.1f", vMin, vMax);
    lv_label_set_text(lbl_minmax, bufMM);
}
