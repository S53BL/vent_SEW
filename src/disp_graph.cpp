// disp_graph.cpp - LVGL graf za vent_SEW
//
// POPRAVKI (2026-03-03, korak 3):
//   - Lokalni history[] RAM buffer ODSTRANJEN — podatki so v graph_store.cpp
//   - graphRefresh() bere iz gsHistory[] prek graphStoreGet()
//   - Časovno okno (currentGraphHours) filtrira točke po timestamp
//   - Swipe levo/desno: menja sensor; kratek dotik: zoom in; dolgi: zoom out
//   - calcYRange(): dinamična Y os z minimalnim razponom (preprečuje "drama")
//   - NVS persistenca: saveGraphPrefs() / loadGraphPrefs()
//   - LVGL API: direkten dostop ser->y_points[i] — lv_chart_set_value_by_id() ne obstaja v tej verziji LVGL

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
static lv_obj_t*          lbl_name     = nullptr;  // ime senzorja + ure (levo, glava)
static lv_obj_t*          lbl_minmax   = nullptr;  // min/max (desno, glava)

// Oznake osi (3x Y, 3x X)
static lv_obj_t*          lbl_ymax     = nullptr;  // Y os: zgornja vrednost
static lv_obj_t*          lbl_ymid     = nullptr;  // Y os: srednja vrednost
static lv_obj_t*          lbl_ymin     = nullptr;  // Y os: spodnja vrednost
static lv_obj_t*          lbl_xL       = nullptr;  // X os: levo  "-Nh"
static lv_obj_t*          lbl_xM       = nullptr;  // X os: sredina "-N/2h"
static lv_obj_t*          lbl_xR       = nullptr;  // X os: desno "0"

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
            // lbl_name se posodobi spodaj skupaj z urami

        } else if (duration < 500) {
            // KRATEK DOTIK — zoom in (krajše časovno okno)
            graphZoomIn();
        } else {
            // DOLGI DOTIK — zoom out (daljše časovno okno)
            graphZoomOut();
        }

        // Posodobi ime senzorja z novimi urami v glavi
        if (lbl_name) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%s  %dh", graphSensorName(currentGraphSensor), currentGraphHours);
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

    // Glava — 1 vrstica: ime senzorja + ure (levo), min/max (desno), font 16, bela
    lbl_name = lv_label_create(graphCont);
    lv_obj_set_pos(lbl_name, 4, 4);
    lv_obj_set_style_text_color(lbl_name, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_name, &lv_font_montserrat_16, 0);
    {
        char nameBuf[32];
        snprintf(nameBuf, sizeof(nameBuf), "%s  %dh", graphSensorName(currentGraphSensor), currentGraphHours);
        lv_label_set_text(lbl_name, nameBuf);
    }

    lbl_minmax = lv_label_create(graphCont);
    lv_obj_align(lbl_minmax, LV_ALIGN_TOP_RIGHT, -4, 4);
    lv_obj_set_style_text_font(lbl_minmax, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_minmax, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(lbl_minmax, "");

    // -----------------------------------------------------------------------
    // Y osi oznake (3 vrednosti: max, mid, min) — levo od chart-a
    // -----------------------------------------------------------------------
    // Chart začne pri x=26 (prostor za Y oznake) in y=40
    // Chart dimenzije: (w-34) × (h-62)
    // Za w=236, h=186: chart = 202×124, Y oznake na x=0..24
    const lv_font_t* fAxis  = &lv_font_montserrat_14;
    lv_color_t       cAxis  = lv_color_hex(0xFFFFFF);
    int chartX = 32, chartY = 24;
    int chartW = w - 40, chartH = h - 48;

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

    lbl_ymax = makeAxisLbl(0, chartY,                        chartX - 2, "--");
    lbl_ymid = makeAxisLbl(0, chartY + chartH / 2 - 5,      chartX - 2, "--");
    lbl_ymin = makeAxisLbl(0, chartY + chartH - 10,         chartX - 2, "--");

    // -----------------------------------------------------------------------
    // X osi oznake (3 vrednosti: -Nh, -N/2h, 0) — pod chart-om
    // -----------------------------------------------------------------------
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

    // -----------------------------------------------------------------------
    // LVGL chart widget — pomaknjeno desno za Y os oznake
    // -----------------------------------------------------------------------
    chart = lv_chart_create(graphCont);
    lv_obj_set_pos(chart, chartX, chartY);
    lv_obj_set_size(chart, chartW, chartH);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, DISPLAY_POINTS_MAX);
    lv_chart_set_div_line_count(chart, 3, 0);
    lv_obj_set_style_bg_color(chart, lv_color_hex(0x111120), 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_line_color(chart, lv_color_hex(0x2A2A40), 0);
    lv_obj_set_style_size(chart, 0, LV_PART_INDICATOR);

    ser = lv_chart_add_series(chart, graphSensorColor(currentGraphSensor), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(chart, ser, LV_CHART_POINT_NONE);

    // Dotik na chart mora se propagirati navzgor do graphCont kjer je handler registriran
    lv_obj_add_flag(chart, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(chart, LV_OBJ_FLAG_EVENT_BUBBLE);

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
            ser->y_points[i] = (lv_coord_t)motionBuckets[bucketFromEnd];
        }
        lv_chart_refresh(chart);

        // Y osi oznake za stolpčni graf (0 .. yMax/2 .. yMax)
        char bufY[8];
        snprintf(bufY, sizeof(bufY), "%d", yMax);     lv_label_set_text(lbl_ymax, bufY);
        snprintf(bufY, sizeof(bufY), "%d", yMax / 2); lv_label_set_text(lbl_ymid, bufY);
        lv_label_set_text(lbl_ymin, "0");

        // X osi oznake
        char xbuf[8];
        snprintf(xbuf, sizeof(xbuf), "-%dh", currentGraphHours);     lv_label_set_text(lbl_xL, xbuf);
        snprintf(xbuf, sizeof(xbuf), "-%dh", currentGraphHours / 2); lv_label_set_text(lbl_xM, xbuf);
        lv_label_set_text(lbl_xR, "0");

        // Glava: max v/h
        char bufCur[16];
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

    // --- 8. Zapiši točke v LVGL (direkten dostop — lv_chart_set_value_by_id ne obstaja v tej verziji) ---
    lv_chart_set_all_value(chart, ser, LV_CHART_POINT_NONE);
    for (int i = 0; i < validPts; i++) {
        ser->y_points[i] = vals[i];
    }
    lv_chart_refresh(chart);

    // --- 9. Posodobi glavo: min/max ---
    char bufMM[32];
    snprintf(bufMM, sizeof(bufMM), "%.1f / %.1f", vMin, vMax);
    lv_label_set_text(lbl_minmax, bufMM);

    // --- 10. Posodobi oznake osi ---
    // Y osi: min, mid, max (zaokroženi)
    char bufY[12];
    float yMidVal = (yMin + yMax) / 2.0f;
    snprintf(bufY, sizeof(bufY), "%.0f", yMax);    lv_label_set_text(lbl_ymax, bufY);
    snprintf(bufY, sizeof(bufY), "%.0f", yMidVal); lv_label_set_text(lbl_ymid, bufY);
    snprintf(bufY, sizeof(bufY), "%.0f", yMin);    lv_label_set_text(lbl_ymin, bufY);

    // X osi: relativni čas -Nh, -N/2h, 0
    char xbuf[8];
    snprintf(xbuf, sizeof(xbuf), "-%dh", currentGraphHours);     lv_label_set_text(lbl_xL, xbuf);
    snprintf(xbuf, sizeof(xbuf), "-%dh", currentGraphHours / 2); lv_label_set_text(lbl_xM, xbuf);
    lv_label_set_text(lbl_xR, "0");
}
