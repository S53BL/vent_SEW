// disp_graph.cpp - LVGL graf za vent_SEW (graph_update.md 2026-03-04)
//
// SPREMEMBE glede na prejšnjo verzijo:
//   - GraphStorePoint + gsHistory → GraphPoint + graphGetHistoryOrdered()
//   - GRAPH_MOTION UKINJEN (§5 spec)
//   - GRAPH_LUX (3) + GRAPH_CLOUD (5) DODANI
//   - Moving average (§5.2): 3-točkovno drsno povprečje
//   - DISPLAY_POINTS = 240 (iz disp_graph.h), chart point count posodobljen
//   - GRAPH_WIND + GRAPH_CLOUD bereta iz GraphPoint (ne special weather veja)
//   - LVGL API: direkten dostop ser->y_points[] (lv_chart_set_value_by_id ne obstaja)
//
// FIX 2026-03-04: Časovna normalizacija v graphRefresh()
//   - PREJ: "fill from right" — točke se natresejo od desne brez upoštevanja časa
//           → zoom 2h je videti enako kot 24h (krivulja skupaj na desni)
//   - ZDAJ: vsak od 240 pikslov dobi vrednost glede na timestamp
//           → pri 2h zoomu (40 točk/6px per pt) krivulja zapolni celo širino
//           → pri 24h (480 točk/0.5px per pt) kompresija z nearest-neighbor
//   - Moving average se zdaj izvaja na normaliziranem (240px) arraju, ne na surovih točkah
//
// UI REFACTOR 2026-03-04: header, barvne cone, X/Y osi
//   - 2-vrstični header z ločeno header cono (#1C1C30), chart cone (#0A0A18)
//   - ime senzorja v barvi krivulje (vrstica 1, font16)
//   - min/max skrajšano v vrstici 2 (font14, lux → k-notacija)
//   - "Xh" iz naslova odstranjeno — X os že prikazuje čas
//   - X markerji: 5 namesto 3, brez "h" (samo število), pravilno pozicionirani
//   - Y os: širša (chartX=36), vrednosti skrajšane za lux
//   - Barvne cone: headerPane #1C1C30, chart #0A0A18, separator #333355
//
// PIXEL LAYOUT (w=236, h=186):
//   y=  0..39  headerPane  #1C1C30  (h=39)
//     y=  4    lbl_name    font16   barva senzorja
//     y= 22    lbl_minmax  font14   #9090B0  (dim blue-grey)
//   y= 39      separator   1px      #333355
//   y= 40..169 chart       #0A0A18  chartX=36, chartW=196, chartH=130
//   y=172..186 X osi       font14   #9090B0  (5 markerjev, brez "h")

#include "disp_graph.h"
#include "graph_store.h"
#include "globals.h"
#include "logging.h"
#include <Preferences.h>
#include <time.h>
#include <math.h>

// -----------------------------------------------------------------------
// LAYOUT KONSTANTE
// Vse pozicije so zbrane tukaj za enostavno korigiranje.
// Sprememba katerekoli vrednosti NE zahteva iskanja po kodi.
// -----------------------------------------------------------------------

// Širina Y osi (levo od charta) — mora biti dovolj za "9.9k" pri font14
// font14 znak ≈ 7px, 4 znaki + 1 presledek = 29px → 36px je v redu
#define LAYOUT_CHART_X       36    // px od levega roba graphCont do charta

// Višina header cone (2 vrstici: ime + min/max)
// Vrstica 1 (font16): y=4, h=18 → spodnji rob y=22
// Vrstica 2 (font14): y=22, h=16 → spodnji rob y=38
// Header skupaj: 39px (1px separator sledi)
#define LAYOUT_HEADER_H      39    // px višina header cone

// Prostor za X osi oznake pod chartom
// font14 h=14px + 2px padding zgoraj + 0px spodaj = 16px
// xAxisY = chartY + chartH + 2 → 2px med chartom in teksti
#define LAYOUT_XAXIS_H       16    // px rezervirano za X osi tekste

// Separator med headerjem in chartom: 1px linija #333355
// Nastavi se kot spodnja meja headerPane z border_bottom
#define LAYOUT_SEP_H          1    // px separator (del headerPane)

// Desni odmik znotraj graphCont (margina na desni strani)
#define LAYOUT_RIGHT_MARGIN   4    // px

// Maksimalno dovoljeno časovno odstopanje od meritve do piksla [s]
// 1.5 × 3min interval = 270s → piksli brez meritve znotraj te razdalje
// dobijo vrednost, piksli dlje od tega → NONE (vidna vrzel v krivulji)
#define GRAPH_MAX_GAP_S     270UL  // sekunde

// -----------------------------------------------------------------------
// BARVNA SHEMA (strogo dark)
// Vse barve so zbrane tukaj — enostavno prilagajanje teme.
// -----------------------------------------------------------------------
#define COL_HEADER_BG    0x1C1C30  // header cona — temno modro-vijolična
#define COL_CHART_BG     0x0A0A18  // chart cona  — skoraj črna z modrim tonom
#define COL_CONT_BG      0x141422  // container bg — vmesni ton (viden le kot rob)
#define COL_CONT_BORDER  0x333355  // rob in separator — moder odtenek
#define COL_AXIS_TEXT    0x9090B0  // tekst X osi in min/max — dim blue-grey
#define COL_GRID_LINE    0x1E1E38  // mrežne linije znotraj charta — komaj vidne
#define COL_AXIS_Y_TEXT  0x9090B0  // tekst Y osi — enako kot X osi

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

// -----------------------------------------------------------------------
// Skaliranje za LVGL (lv_coord_t je integer, 16-bit signed max 32767)
// Vrednosti pomnožimo s scale pred vpisom v ser->y_points[].
// Primer: GRAPH_TEMP scale=10 → 21.3°C postane 213 (integer)
// -----------------------------------------------------------------------
static int graphScale(int sensor) {
    switch (sensor) {
        case GRAPH_TEMP:  return 10;  // 21.3°C → 213
        case GRAPH_HUM:   return 10;  // 65.4%  → 654
        case GRAPH_IAQ:   return 1;   // 0–500  → direktno
        case GRAPH_LUX:   return 1;   // 0–xxxxx → direktno (velike vrednosti)
        case GRAPH_WIND:  return 10;  // 12.5   → 125
        case GRAPH_CLOUD: return 1;   // 0–100  → direktno
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
// Vsi se inicializirajo v initGraph() in nikoli ne postanejo nullptr po init.
// -----------------------------------------------------------------------
static lv_obj_t*          graphCont   = nullptr;  // zunanji container (cel graf)
static lv_obj_t*          headerPane  = nullptr;  // header zona (#1C1C30)
static lv_obj_t*          chart       = nullptr;  // LVGL chart widget
static lv_chart_series_t* ser         = nullptr;  // podatkovna serija v chartu
static lv_obj_t*          lbl_name    = nullptr;  // vrstica 1: kratko ime senzorja (barva krivulje)
static lv_obj_t*          lbl_minmax  = nullptr;  // vrstica 2: min / max [unit] skrajšano
static lv_obj_t*          lbl_ymax    = nullptr;  // Y os: maksimalna vrednost (zgoraj)
static lv_obj_t*          lbl_ymid    = nullptr;  // Y os: srednja vrednost
static lv_obj_t*          lbl_ymin    = nullptr;  // Y os: minimalna vrednost (spodaj)
static lv_obj_t*          lbl_xL      = nullptr;  // X os: levi marker  (0%)
static lv_obj_t*          lbl_xQ1     = nullptr;  // X os: 25% marker
static lv_obj_t*          lbl_xM      = nullptr;  // X os: 50% marker (sredina)
static lv_obj_t*          lbl_xQ3     = nullptr;  // X os: 75% marker
static lv_obj_t*          lbl_xR      = nullptr;  // X os: desni marker (100% = "0")

// -----------------------------------------------------------------------
// Touch detekcija (swipe = sensor, tap = zoom)
// -----------------------------------------------------------------------
static lv_point_t touchStart;
static uint32_t   touchStartMs = 0;

// -----------------------------------------------------------------------
// Sensor metapodatki — polna imena (za debug in morebitne namene)
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

// -----------------------------------------------------------------------
// Kratka imena za header (vrstica 1) — prilagodi glede na prostor
// Maksimalna dolžina: ~8 znakov pri font16 (~128px) v 236px širini
// -----------------------------------------------------------------------
static const char* graphSensorShortName(int idx) {
    switch (idx) {
        case GRAPH_TEMP:  return "Temp";    // 4 znake
        case GRAPH_HUM:   return "Vlaznost"; // 8 znakov — OK
        case GRAPH_IAQ:   return "IAQ";
        case GRAPH_LUX:   return "Svetloba"; // 8 znakov — OK
        case GRAPH_WIND:  return "Veter";
        case GRAPH_CLOUD: return "Oblacnost"; // 9 znakov — robno, a OK pri f16
        default:          return "?";
    }
}

// -----------------------------------------------------------------------
// Enote za prikaz
// -----------------------------------------------------------------------
const char* graphSensorUnit(int idx) {
    switch (idx) {
        case GRAPH_TEMP:  return "\xC2\xB0""C";  // °C (UTF-8)
        case GRAPH_HUM:   return "%";
        case GRAPH_IAQ:   return "";
        case GRAPH_LUX:   return "lx";
        case GRAPH_WIND:  return "km/h";
        case GRAPH_CLOUD: return "%";
        default:          return "";
    }
}

// -----------------------------------------------------------------------
// Barve krivulj — ena barva na sensor za identifikacijo brez da bereš tekst
// -----------------------------------------------------------------------
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
// formatYVal — formatira vrednost za Y os oznake
//
// Lux senzor vrača vrednosti do 100000+ lx, ki ne vstopijo v 36px kolono
// pri font14. "k-notacija" skrajša: 1234 → "1.2k", 12345 → "12k"
// Ostali senzorji imajo vrednosti do max ~500 → "%.0f" zadostuje.
//
// Klic: formatYVal(buf, sizeof(buf), value, currentGraphSensor)
// -----------------------------------------------------------------------
static void formatYVal(char* buf, int sz, float val, int sensor) {
    if (sensor == GRAPH_LUX && fabsf(val) >= 1000.0f) {
        // k-notacija: 1234 → "1.2k", 12345 → "12k"
        float k = val / 1000.0f;
        if (k < 10.0f)
            snprintf(buf, sz, "%.1fk", k);   // "1.2k" (4 znake)
        else
            snprintf(buf, sz, "%.0fk", k);   // "12k" (3 znake)
    } else {
        snprintf(buf, sz, "%.0f", val);      // "213", "-5", "100" itd.
    }
}

// -----------------------------------------------------------------------
// formatMinMax — formatira min/max za vrstico 2 headerja
//
// Primer: "21.4 / 24.1 °C"
//         "1.2k / 3.8k lx"   (lux k-notacija)
//         "45 / 120 IAQ"
// -----------------------------------------------------------------------
static void formatMinMax(char* buf, int sz, float vMin, float vMax, int sensor) {
    const char* unit = graphSensorUnit(sensor);

    if (sensor == GRAPH_LUX && vMax >= 1000.0f) {
        // Obe vrednosti v k-notaciji
        char sMin[8], sMax[8];
        formatYVal(sMin, sizeof(sMin), vMin, sensor);
        formatYVal(sMax, sizeof(sMax), vMax, sensor);
        snprintf(buf, sz, "%s / %s %s", sMin, sMax, unit);
    } else if (sensor == GRAPH_TEMP || sensor == GRAPH_HUM || sensor == GRAPH_WIND) {
        // 1 decimalno mesto za vrednosti s plavajočo vejico
        snprintf(buf, sz, "%.1f / %.1f %s", vMin, vMax, unit);
    } else {
        // IAQ, CLOUD: cela števila
        snprintf(buf, sz, "%.0f / %.0f %s", vMin, vMax, unit);
    }
}

// -----------------------------------------------------------------------
// calcYRange — dinamična Y os z minimalnim razponom
// Dela v naravnih enotah (pred skaliranjem).
// Zagotavlja, da razpon ni nikoli premajhen (izogibu "zoom drama").
// -----------------------------------------------------------------------
static void calcYRange(int sensor, float vMin, float vMax,
                       float& yMin, float& yMax) {
    float range    = vMax - vMin;
    float minRange = (sensor >= 0 && sensor < GRAPH_COUNT)
                     ? Y_MIN_RANGE[sensor] : 1.0f;

    if (range < minRange) {
        // Premajhen razpon → razširi simetrično okoli sredine
        float center = (vMax + vMin) / 2.0f;
        yMin = center - minRange / 2.0f;
        yMax = center + minRange / 2.0f;
    } else {
        // Normalen razpon → dodaj 10% margine na vsako stran
        yMin = vMin - range * 0.10f;
        yMax = vMax + range * 0.10f;
    }
    // Zaokroži na cela števila (bolj pregledne oznake na Y osi)
    yMin = floorf(yMin);
    yMax = ceilf(yMax);
}

// -----------------------------------------------------------------------
// NVS persistenca — shrani/naloži currentGraphSensor in currentGraphHours
// -----------------------------------------------------------------------
void loadGraphPrefs() {
    Preferences prefs;
    prefs.begin(GRAPH_NVS_NS, true);
    currentGraphSensor = prefs.getInt("sensor", GRAPH_TEMP);
    currentGraphHours  = prefs.getInt("hours",  4);
    prefs.end();

    // Validacija — prepreči neveljavne vrednosti po korupciji NVS
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
// Zoom — kroži med 2h → 4h → 8h → 16h → 24h
// graphZoomIn(): zmanjša okno (bližje)
// graphZoomOut(): poveča okno (dlje)
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
    // Že na minimumu (2h) — ne naredi nič
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
    // Že na maksimumu (24h) — ne naredi nič
}

// -----------------------------------------------------------------------
// Touch event handler
// Swipe levo/desno (dx > 40px):  menja senzor
// Kratek tap (< 500ms):         zoom in
// Dolg tap (≥ 500ms):           zoom out
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
            // Posodobi barvo serije takoj (brez čakanja na refresh)
            if (chart && ser)
                lv_chart_set_series_color(chart, ser, graphSensorColor(currentGraphSensor));
        } else if (abs(dx) <= 40) {
            // TAP — zoom glede na trajanje
            if (duration < 500) graphZoomIn();
            else                 graphZoomOut();
        }

        saveGraphPrefs();
        graphRefresh();  // posodobi vse labele in krivuljo
    }
}

// -----------------------------------------------------------------------
// initGraph — ustvari vse LVGL widgete za grafični prikaz
//
// Kliči enkrat iz initDisplay() v disp.cpp.
// Preference (sensor, hours) se naložijo v main.cpp PO tem klicu.
//
// Parametre (x, y, w, h) poda klic iz disp.cpp:
//   initGraph(bot_panel, 2, 2, SCR_W-4, BOT_H-4)
//   → x=2, y=2, w=236, h=186
// -----------------------------------------------------------------------
void initGraph(lv_obj_t* parent, int x, int y, int w, int h) {
    // ---------------------------------------------------------------
    // Izračun layout vrednosti iz w in h ter LAYOUT_* konstant
    // Vse derived vrednosti so lokalne spremenljivke, ne #define,
    // da so odvisnosti vidne in enostavne za debug.
    // ---------------------------------------------------------------

    // X os: leva meja charta (Y osi oznake so levo od te meje)
    const int chartX  = LAYOUT_CHART_X;          // 36px

    // Y os: zgornja meja charta (header + separator so nad njo)
    const int chartY  = LAYOUT_HEADER_H + LAYOUT_SEP_H;  // 39 + 1 = 40px

    // Širina charta: w minus Y osi kolona levo, minus desna margina
    const int chartW  = w - chartX - LAYOUT_RIGHT_MARGIN;  // 236-36-4 = 196px

    // Višina charta: h minus header, minus X osi prostor spodaj
    // Dodatni 2px odbitek: padding med chartom in X teksti (xAxisY = chartY+chartH+2)
    const int chartH  = h - chartY - LAYOUT_XAXIS_H - 2;  // 186-40-16-2 = 128px

    // Y pozicija X osi oznak (2px pod spodnjim robom charta)
    const int xAxisY  = chartY + chartH + 2;              // 170px

    // Vsak X marker je 1/4 chartW od prejšnjega
    const int xStep   = chartW / 4;                        // 196/4 = 49px

    // Fonti za posamezne elemente
    // LAYOUT: sprememba fonte tukaj vpliva na vse elemente iste cone
    const lv_font_t* fHeader = &lv_font_montserrat_16;  // ime senzorja
    const lv_font_t* fSub    = &lv_font_montserrat_14;  // min/max, osi
    const lv_font_t* fAxis   = &lv_font_montserrat_14;  // Y in X osi oznake

    // ---------------------------------------------------------------
    // ZUNANJI CONTAINER (graphCont)
    // Barva: vmesni dark ton, viden le kot 1px rob
    // ---------------------------------------------------------------
    graphCont = lv_obj_create(parent);
    lv_obj_set_pos(graphCont, x, y);
    lv_obj_set_size(graphCont, w, h);
    lv_obj_set_style_bg_color(graphCont, lv_color_hex(COL_CONT_BG), 0);
    lv_obj_set_style_border_color(graphCont, lv_color_hex(COL_CONT_BORDER), 0);
    lv_obj_set_style_border_width(graphCont, 1, 0);
    lv_obj_set_style_radius(graphCont, 8, 0);
    lv_obj_set_style_pad_all(graphCont, 0, 0);   // brez notranjega paddinga
    lv_obj_clear_flag(graphCont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(graphCont, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(graphCont, graphTouchHandler, LV_EVENT_PRESSED,  nullptr);
    lv_obj_add_event_cb(graphCont, graphTouchHandler, LV_EVENT_RELEASED, nullptr);

    // ---------------------------------------------------------------
    // HEADER PANE (headerPane)
    // Ločena barvna cona nad chartom. Barva: #1C1C30 (temno vijolično-modra)
    // Spodnji rob: 1px separator v barvi #333355
    // ---------------------------------------------------------------
    headerPane = lv_obj_create(graphCont);
    lv_obj_set_pos(headerPane, 0, 0);
    lv_obj_set_size(headerPane, w, LAYOUT_HEADER_H);
    lv_obj_set_style_bg_color(headerPane, lv_color_hex(COL_HEADER_BG), 0);
    lv_obj_set_style_border_color(headerPane, lv_color_hex(COL_CONT_BORDER), 0);
    lv_obj_set_style_border_width(headerPane, 0, 0);
    // Spodnji separator: 1px border na dnu headerPane
    lv_obj_set_style_border_side(headerPane, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(headerPane, 1, 0);
    lv_obj_set_style_radius(headerPane, 0, 0);
    lv_obj_set_style_pad_all(headerPane, 0, 0);
    lv_obj_clear_flag(headerPane, LV_OBJ_FLAG_SCROLLABLE);
    // Touch propagira do graphCont
    lv_obj_add_flag(headerPane, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(headerPane, LV_OBJ_FLAG_EVENT_BUBBLE);

    // ---------------------------------------------------------------
    // HEADER VRSTICA 1: kratko ime senzorja (v barvi krivulje)
    // Pozicija: y=4px od vrha, x=8px od leve (po headerPanetx)
    // Barva se za posodobi v graphRefresh() ob vsaki zamenjavi senzorja.
    // ---------------------------------------------------------------
    lbl_name = lv_label_create(headerPane);
    lv_obj_set_pos(lbl_name, 8, 4);
    lv_obj_set_style_text_font(lbl_name, fHeader, 0);
    lv_obj_set_style_text_color(lbl_name, graphSensorColor(currentGraphSensor), 0);
    lv_label_set_text(lbl_name, graphSensorShortName(currentGraphSensor));

    // ---------------------------------------------------------------
    // HEADER VRSTICA 2: min / max vrednosti (skrajšano, barva #9090B0)
    // Pozicija: neposredno pod vrstico 1
    // Vrednost "Ni podatkov" ob zagonu (preden prispe prva meritev)
    // ---------------------------------------------------------------
    lbl_minmax = lv_label_create(headerPane);
    lv_obj_set_pos(lbl_minmax, 8, 22);
    lv_obj_set_style_text_font(lbl_minmax, fSub, 0);
    lv_obj_set_style_text_color(lbl_minmax, lv_color_hex(COL_AXIS_TEXT), 0);
    lv_label_set_text(lbl_minmax, "Ni podatkov");

    // ---------------------------------------------------------------
    // Y OSI OZNAKE (lbl_ymax, lbl_ymid, lbl_ymin)
    // Pozicija: levo od charta (x=0, širina = chartX-2 = 34px)
    // Desno poravnano, font14
    // Vrednosti se posodobijo v graphRefresh().
    // ---------------------------------------------------------------
    auto makeYLbl = [&](int ay) -> lv_obj_t* {
        lv_obj_t* l = lv_label_create(graphCont);
        lv_obj_set_pos(l, 0, ay);
        lv_obj_set_width(l, chartX - 2);   // 34px — tesno ob levi rob charta
        lv_obj_set_style_text_font(l, fAxis, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(COL_AXIS_Y_TEXT), 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_text(l, "--");
        return l;
    };

    // Pozicije Y oznak: max=vrh charta, mid=sredina, min=dno charta
    // Offset -5px pri ymid/ymin: font je visok ~14px, centriramo na linijo
    lbl_ymax = makeYLbl(chartY);
    lbl_ymid = makeYLbl(chartY + chartH / 2 - 7);
    lbl_ymin = makeYLbl(chartY + chartH - 14);

    // ---------------------------------------------------------------
    // X OSI OZNAKE — 5 markerjev (0%, 25%, 50%, 75%, 100%)
    // Brez "h" — jasno je, da gre za ure
    // Primer zoom 8h: "-8  -6  -4  -2  0"
    // Pozicija Y: 2px pod spodnjim robom charta (xAxisY = chartY + chartH + 2)
    // Lbl_xR: označba "0" — trdno zgoraj desno pri x = chartX + chartW - 8
    // ---------------------------------------------------------------
    auto makeXLbl = [&](int ax) -> lv_obj_t* {
        lv_obj_t* l = lv_label_create(graphCont);
        lv_obj_set_pos(l, ax, xAxisY);
        lv_obj_set_style_text_font(l, fAxis, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(COL_AXIS_TEXT), 0);
        lv_label_set_text(l, "--");
        return l;
    };

    // lbl_xL: levi rob charta (najstarejša točka)
    lbl_xL  = makeXLbl(chartX);
    // lbl_xQ1: 25% od leve (offset -8px da besedilo lepo centriramo)
    lbl_xQ1 = makeXLbl(chartX + xStep - 8);
    // lbl_xM: sredina (offset -10px za centering)
    lbl_xM  = makeXLbl(chartX + xStep * 2 - 10);
    // lbl_xQ3: 75% (offset -8px)
    lbl_xQ3 = makeXLbl(chartX + xStep * 3 - 8);
    // lbl_xR: desni rob (offset -8px od desnega roba charta)
    lbl_xR  = makeXLbl(chartX + chartW - 8);

    // ---------------------------------------------------------------
    // LVGL CHART WIDGET
    // Cona: #0A0A18 (skoraj črna z modrim tonom) — kontrast do headra
    // Mrežne linije: #1E1E38 (komaj vidne — nočemo motiti krivulje)
    // ---------------------------------------------------------------
    chart = lv_chart_create(graphCont);
    lv_obj_set_pos(chart, chartX, chartY);
    lv_obj_set_size(chart, chartW, chartH);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, DISPLAY_POINTS);   // 240 točk = 1px/točka pri 240px
    // 3 vodoravne mrežne linije (top/mid/bottom) — usklajene z Y oznakami
    lv_chart_set_div_line_count(chart, 3, 0);
    // Barve widgeta
    lv_obj_set_style_bg_color(chart, lv_color_hex(COL_CHART_BG), 0);
    lv_obj_set_style_border_width(chart, 0, 0);         // brez obrobe charta
    lv_obj_set_style_line_color(chart, lv_color_hex(COL_GRID_LINE), 0);  // mrežne linije
    lv_obj_set_style_size(chart, 0, LV_PART_INDICATOR); // brez pik na meritev

    // Podatkovna serija v barvi trenutnega senzorja
    ser = lv_chart_add_series(chart, graphSensorColor(currentGraphSensor), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(chart, ser, LV_CHART_POINT_NONE);  // prazno do prve meritve

    // Touch na chartu propagira navzgor do graphCont
    lv_obj_add_flag(chart, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(chart, LV_OBJ_FLAG_EVENT_BUBBLE);

    LOG_INFO("GRAPH", "Graph init: container=%dx%d chart=%dx%d@(%d,%d) xAxisY=%d",
             w, h, chartW, chartH, chartX, chartY, xAxisY);
}

// -----------------------------------------------------------------------
// graphRefresh — osveži graf iz graph_store
//
// Algoritem (časovna normalizacija):
//   1. graphGetHistoryOrdered() → točke v kronološkem vrstnem redu
//   2. Filtriraj točke v časovnem oknu (tStart..now)
//   3. Za vsak od 240 pikslov izračunaj ciljni timestamp
//      → poišči najbližjo meritev (nearest-neighbor)
//      → piksli brez meritve v GRAPH_MAX_GAP_S → NONE (vidna vrzel)
//   4. Moving average (3 točke) na normaliziranem arraju
//   5. Zapiši v ser->y_points[], posodobi osi in labele
// -----------------------------------------------------------------------
void graphRefresh() {
    if (!chart || !ser) return;

    // --- 1. Pridobi točke iz ring bufferja v kronološkem vrstnem redu ---
    static GraphPoint histBuf[GRAPH_HISTORY_MAX];
    int total = graphGetHistoryOrdered(histBuf, GRAPH_HISTORY_MAX);

    // --- 2. Določi časovno okno ---
    uint32_t now    = (uint32_t)time(nullptr);
    uint32_t tStart = now - (uint32_t)(currentGraphHours * 3600UL);
    uint32_t tSpan  = (uint32_t)(currentGraphHours * 3600UL);

    const float NONE_SENTINEL = -998.0f;  // vrednost za "ni meritve"

    // --- 3. Izvleči (ts, value) pare za aktivni senzor ---
    // Samo točke znotraj trenutnega časovnega okna.
    // Statična alokacija — brez heap fragmentacije.
    static uint32_t ptTs[GRAPH_HISTORY_MAX];
    static float    ptVal[GRAPH_HISTORY_MAX];
    int rawCount = 0;
    float vMin =  1e9f;
    float vMax = -1e9f;

    for (int i = 0; i < total; i++) {
        const GraphPoint& pt = histBuf[i];
        if (pt.ts < tStart || pt.ts > now) continue;  // izven okna

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

        if (v <= NONE_SENTINEL) continue;  // preskoči neveljavne vrednosti

        ptTs[rawCount]  = pt.ts;
        ptVal[rawCount] = v;
        rawCount++;

        if (v < vMin) vMin = v;
        if (v > vMax) vMax = v;
    }

    // --- 4. Premalo podatkov za smiselni prikaz ---
    if (rawCount < 2) {
        lv_chart_set_all_value(chart, ser, LV_CHART_POINT_NONE);
        if (lbl_minmax) lv_label_set_text(lbl_minmax, "Ni podatkov");
        lv_chart_refresh(chart);
        return;
    }

    // --- 5. Časovna normalizacija: pixel → timestamp → vrednost ---
    // Vsak od DISPLAY_POINTS pikslov dobi vrednost glede na čas.
    // Piksel 0 = tStart (najstarejši), piksel 239 = now (najnovejši).
    // secPerPx: koliko sekund pokriva en piksel.
    static float normVals[DISPLAY_POINTS];

    const float secPerPx = (float)tSpan / (float)(DISPLAY_POINTS - 1);

    // searchStart: optimizacija linearnega iskanja — ker sta ptTs[] in px
    // oba naraščajoča v času, ni treba vedno začeti od 0.
    int searchStart = 0;

    for (int px = 0; px < DISPLAY_POINTS; px++) {
        // Ciljni timestamp za ta piksel
        uint32_t tPx = tStart + (uint32_t)(px * secPerPx + 0.5f);

        // Nearest-neighbor iskanje v ptTs[] od searchStart naprej
        float    bestVal = NONE_SENTINEL;
        uint32_t bestDt  = 0xFFFFFFFFUL;

        for (int j = searchStart; j < rawCount; j++) {
            uint32_t dt = (ptTs[j] >= tPx) ? (ptTs[j] - tPx) : (tPx - ptTs[j]);

            if (dt < bestDt) {
                bestDt  = dt;
                bestVal = ptVal[j];
                // Optimizacija: točke pred tPx so kandidati za searchStart
                if (ptTs[j] <= tPx) searchStart = j;
            }
            // Prekinemo, ko smo že precej za tPx
            if (ptTs[j] > tPx + GRAPH_MAX_GAP_S) break;
        }

        // Vrednost piksla: veljavna le če je meritev dovolj blizu
        normVals[px] = (bestDt <= GRAPH_MAX_GAP_S) ? bestVal : NONE_SENTINEL;
    }

    // --- 6. Moving average (3 točke) na normaliziranem arraju ---
    // Glajenje dela na 240-px arraju, ne na surovih točkah.
    // Ne gladi čez vrzeli (NONE_SENTINEL) — obdrži vidne prekinitve krivulje.
    static float smoothVals[DISPLAY_POINTS];
    smoothVals[0]                = normVals[0];
    smoothVals[DISPLAY_POINTS-1] = normVals[DISPLAY_POINTS-1];

    for (int i = 1; i < DISPLAY_POINTS - 1; i++) {
        if (normVals[i-1] <= NONE_SENTINEL ||
            normVals[i]   <= NONE_SENTINEL ||
            normVals[i+1] <= NONE_SENTINEL) {
            smoothVals[i] = normVals[i];  // vrzel — ne gladi
        } else {
            smoothVals[i] = (normVals[i-1] + normVals[i] + normVals[i+1]) / 3.0f;
        }
    }

    // --- 7. Izračunaj dinamično Y os ---
    float yMin, yMax;
    calcYRange(currentGraphSensor, vMin, vMax, yMin, yMax);

    int scale = graphScale(currentGraphSensor);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y,
                       (lv_coord_t)(yMin * scale),
                       (lv_coord_t)(yMax * scale));

    // --- 8. Zapiši v ser->y_points[] (direkten dostop, pixel-po-pixel) ---
    lv_chart_set_all_value(chart, ser, LV_CHART_POINT_NONE);
    for (int px = 0; px < DISPLAY_POINTS; px++) {
        if (smoothVals[px] <= NONE_SENTINEL) {
            ser->y_points[px] = LV_CHART_POINT_NONE;     // vrzel → brez točke
        } else {
            ser->y_points[px] = (lv_coord_t)(smoothVals[px] * scale);
        }
    }
    lv_chart_refresh(chart);

    // --- 9. Posodobi serijo barvo (morda se je senzor zamenjal) ---
    lv_chart_set_series_color(chart, ser, graphSensorColor(currentGraphSensor));

    // --- 10. Header vrstica 1: kratko ime senzorja v barvi krivulje ---
    if (lbl_name) {
        lv_label_set_text(lbl_name, graphSensorShortName(currentGraphSensor));
        lv_obj_set_style_text_color(lbl_name, graphSensorColor(currentGraphSensor), 0);
    }

    // --- 11. Header vrstica 2: min / max (skrajšano, z enoto) ---
    if (lbl_minmax) {
        char bufMM[32];
        formatMinMax(bufMM, sizeof(bufMM), vMin, vMax, currentGraphSensor);
        lv_label_set_text(lbl_minmax, bufMM);
    }

    // --- 12. Y osi oznake (max / mid / min) ---
    // Sredinska vrednost: aritmetična sredina Y osi (ne nujno sredina podatkov)
    char bufY[10];
    float yMidVal = (yMin + yMax) / 2.0f;
    if (lbl_ymax) { formatYVal(bufY, sizeof(bufY), yMax,    currentGraphSensor); lv_label_set_text(lbl_ymax, bufY); }
    if (lbl_ymid) { formatYVal(bufY, sizeof(bufY), yMidVal, currentGraphSensor); lv_label_set_text(lbl_ymid, bufY); }
    if (lbl_ymin) { formatYVal(bufY, sizeof(bufY), yMin,    currentGraphSensor); lv_label_set_text(lbl_ymin, bufY); }

    // --- 13. X osi oznake — 5 markerjev brez "h" ---
    // Marker vrednosti: relativni čas glede na "now" (0 = zdaj, negativno = preteklost)
    // Primer zoom 8h: "-8  -6  -4  -2  0"
    // Primer zoom 24h: "-24  -18  -12  -6  0"
    // Primer zoom 2h: "-2  -90m  -1  -30m  0" — pri 2h so intervali 30 min
    // Ker so oznake cela števila ur, pri 2h prikažemo samo "-2" in "0" na robovih,
    // vmesni markerji pa so v minutah → za enostavnost prikažemo decimalne ure.
    {
        char xbuf[8];
        int  h = currentGraphHours;

        // Vrednosti na 0%, 25%, 50%, 75%, 100% osi
        // q = size of each quarter in hours (dapat biti ne-celo pri h=2)
        // Za h>=4: četrtine so cela števila. Za h=2: četrtine = 0.5h → prikažemo min.
        if (h >= 4) {
            // Cela števila ur — enostavno za branje
            snprintf(xbuf, sizeof(xbuf), "-%d", h);        if (lbl_xL)  lv_label_set_text(lbl_xL,  xbuf);
            snprintf(xbuf, sizeof(xbuf), "-%d", h*3/4);    if (lbl_xQ1) lv_label_set_text(lbl_xQ1, xbuf);
            snprintf(xbuf, sizeof(xbuf), "-%d", h/2);      if (lbl_xM)  lv_label_set_text(lbl_xM,  xbuf);
            snprintf(xbuf, sizeof(xbuf), "-%d", h/4);      if (lbl_xQ3) lv_label_set_text(lbl_xQ3, xbuf);
            if (lbl_xR) lv_label_set_text(lbl_xR, "0");
        } else {
            // h=2: četrtine = 30 min → prikažemo v minutah ("-2" "-90m" "-1" "-30m" "0")
            // Oziroma samo "-2", "-1", "0" na robovih in sredini; vmesna dva pusti "--"
            snprintf(xbuf, sizeof(xbuf), "-%d", h);        if (lbl_xL)  lv_label_set_text(lbl_xL,  xbuf);
            if (lbl_xQ1) lv_label_set_text(lbl_xQ1, "-90m");
            if (lbl_xM)  lv_label_set_text(lbl_xM,  "-1");
            if (lbl_xQ3) lv_label_set_text(lbl_xQ3, "-30m");
            if (lbl_xR)  lv_label_set_text(lbl_xR,  "0");
        }
    }

    LOG_DEBUG("GRAPH", "Refresh: sensor=%d(%s) pts=%d/%d zoom=%dh "
              "yMin=%.1f yMax=%.1f secPerPx=%.1f",
              currentGraphSensor, graphSensorShortName(currentGraphSensor),
              rawCount, total, currentGraphHours,
              yMin, yMax, secPerPx);
}
