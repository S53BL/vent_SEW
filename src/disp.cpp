// disp.cpp - Display module for vent_SEW (LVGL, ST7789T3, 240x320 portrait)
//
// Glavni zaslon (main_screen):
//   [0..129]   Zgornji del: ura, datum, WiFi ikona, senzorji (T,H,IAQ,P,lux,PIR)
//   [130..319] Spodnji del: LVGL chart (graf senzorjev, klik = naslednji)
//
// Detail screen:
//   Polni zaslon, 3 sekcije: Senzorji, Vreme (OpenMeteo), Sistem
//   Tap kjerkoli -> nazaj, auto timeout 15s
//
// POPRAVKI (2026-02-28):
//   - sd.humidity → sd.hum (SensorData struct ima polje "hum", ne "humidity")
//   - uint16_t primerjava: iaq/staticIaq z -998.0f zamenjana z bme680Present
//     (uint16_t je vedno >= 0, primerjava z negativno float-om vedno true)
//
#include "disp.h"
#include "disp_graph.h"
#include "globals.h"
#include "weather.h"
#include "logging.h"
#include "colours.h"
#include "config.h"
#include "Display_ST7789.h"
#include "LVGL_Driver.h"
#include <time.h>
#include <WiFi.h>
#include <stdio.h>

static lv_obj_t* main_screen    = nullptr;
static lv_obj_t* detail_screen  = nullptr;
static lv_timer_t* detail_timer = nullptr;

static lv_obj_t* lbl_time    = nullptr;
static lv_obj_t* lbl_date    = nullptr;
static lv_obj_t* lbl_wifi    = nullptr;
static lv_obj_t* lbl_temp    = nullptr;
static lv_obj_t* lbl_hum     = nullptr;
static lv_obj_t* lbl_iaq     = nullptr;
static lv_obj_t* lbl_press   = nullptr;
static lv_obj_t* lbl_lux     = nullptr;
static lv_obj_t* lbl_pir     = nullptr;
static lv_obj_t* lbl_weather = nullptr;

static void top_touch_cb(lv_event_t* e);
static void graph_touch_cb(lv_event_t* e);
static void detail_touch_cb(lv_event_t* e);
static void detail_timer_cb(lv_timer_t* t);

static lv_color_t iaqColor(float iaq) {
    if (iaq <  50) return lv_color_hex(COL_IAQ_EXCELLENT);
    if (iaq < 100) return lv_color_hex(COL_IAQ_GOOD);
    if (iaq < 150) return lv_color_hex(COL_IAQ_MODERATE);
    if (iaq < 200) return lv_color_hex(COL_IAQ_POOR);
    if (iaq < 300) return lv_color_hex(COL_IAQ_BAD);
    return lv_color_hex(COL_IAQ_HAZARDOUS);
}

static lv_color_t wifiColor(int rssi, bool connected) {
    if (!connected) return lv_color_hex(0xFF4444);
    if (rssi > -55) return lv_color_hex(0x44FF88);
    if (rssi > -70) return lv_color_hex(0xFFAA00);
    return lv_color_hex(0xFF6644);
}

void initDisplay() {
    Backlight_Init();
    LCD_Init();
    Set_Backlight(settings.screenBrightness * 100 / 1023);
    Lvgl_Init();

    main_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(main_screen, lv_color_hex(COL_BG), 0);
    lv_obj_clear_flag(main_screen, LV_OBJ_FLAG_SCROLLABLE);

    // ZGORNJI DEL
    lv_obj_t* top_panel = lv_obj_create(main_screen);
    lv_obj_set_pos(top_panel, 0, 0);
    lv_obj_set_size(top_panel, SCR_W, TOP_H);
    lv_obj_set_style_bg_color(top_panel, lv_color_hex(0x0E0E1A), 0);
    lv_obj_set_style_border_width(top_panel, 0, 0);
    lv_obj_set_style_radius(top_panel, 0, 0);
    lv_obj_set_style_pad_all(top_panel, 0, 0);
    lv_obj_clear_flag(top_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(top_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(top_panel, top_touch_cb, LV_EVENT_CLICKED, NULL);

    // Ura
    lbl_time = lv_label_create(top_panel);
    lv_obj_set_pos(lbl_time, 8, 6);
    lv_obj_set_style_text_font(lbl_time, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl_time, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(lbl_time, "--:--");

    // Datum
    lbl_date = lv_label_create(top_panel);
    lv_obj_set_pos(lbl_date, 10, 38); // premaknjen za 2 piksla navzgor zaradi večjega fonta
    lv_obj_set_style_text_font(lbl_date, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_date, lv_color_hex(0xAAAAFF), 0);
    lv_label_set_text(lbl_date, "--.--.----");

    // WiFi ikona (desno zgoraj)
    lbl_wifi = lv_label_create(top_panel);
    lv_obj_set_pos(lbl_wifi, SCR_W - 40, 8);
    lv_obj_set_style_text_font(lbl_wifi, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_wifi, lv_color_hex(0xCCCCCC), 0);
    lv_label_set_text(lbl_wifi, LV_SYMBOL_WIFI);

    // Vremenski simbol
    lbl_weather = lv_label_create(top_panel);
    lv_obj_set_pos(lbl_weather, SCR_W - 60, 26); // premaknjen za 2 piksla navzgor zaradi večjega fonta
    lv_obj_set_style_text_font(lbl_weather, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_weather, lv_color_hex(0x99BBFF), 0);
    lv_label_set_text(lbl_weather, "...");

    // Locilna linija
    lv_obj_t* line1 = lv_obj_create(top_panel);
    lv_obj_set_pos(line1, 0, 58); lv_obj_set_size(line1, SCR_W, 1);
    lv_obj_set_style_bg_color(line1, lv_color_hex(0x2A2A40), 0);
    lv_obj_set_style_border_width(line1, 0, 0); lv_obj_set_style_radius(line1, 0, 0);

    // Senzorji: 2 stolpca, 3 vrstice (y = 64..124)
    // Levo: T, P, Lux   Desno: RH, IAQ, PIR

    auto makeLbl = [&](lv_obj_t* parent, int x, int y, const lv_font_t* font, lv_color_t col, const char* txt) -> lv_obj_t* {
        lv_obj_t* l = lv_label_create(parent);
        lv_obj_set_pos(l, x, y);
        lv_obj_set_style_text_font(l, font, 0);
        lv_obj_set_style_text_color(l, col, 0);
        lv_label_set_text(l, txt);
        return l;
    };

    const lv_font_t* fSmall = &lv_font_montserrat_14; // povečano iz 12 na 14
    const lv_font_t* fVal   = &lv_font_montserrat_16;
    lv_color_t cLbl = lv_color_hex(COL_TEXT_SECONDARY); // uporabi svetlo sivo iz colours.h

    makeLbl(top_panel, 8, 64, fSmall, cLbl, "T");
    lbl_temp = makeLbl(top_panel, 22, 62, fVal, lv_color_hex(0xFF8080), "--.-C");

    makeLbl(top_panel, SCR_W/2, 64, fSmall, cLbl, "RH");
    lbl_hum = makeLbl(top_panel, SCR_W/2+20, 62, fVal, lv_color_hex(0x4DA6FF), "--%");

    makeLbl(top_panel, 8, 86, fSmall, cLbl, "P");
    lbl_press = makeLbl(top_panel, 22, 84, fVal, lv_color_hex(0xA8D8A8), "---- hPa");

    makeLbl(top_panel, SCR_W/2, 86, fSmall, cLbl, "IAQ");
    lbl_iaq = makeLbl(top_panel, SCR_W/2+32, 84, fVal, lv_color_hex(0xFFD700), "---");

    makeLbl(top_panel, 8, 108, fSmall, cLbl, "Lux");
    lbl_lux = makeLbl(top_panel, 35, 106, fVal, lv_color_hex(0xFFF176), "----");

    makeLbl(top_panel, SCR_W/2, 108, fSmall, cLbl, "PIR");
    lbl_pir = makeLbl(top_panel, SCR_W/2+30, 106, fVal, lv_color_hex(COL_TEXT_SECONDARY), LV_SYMBOL_BELL);

    // SPODNJI DEL (graf)
    lv_obj_t* bot_panel = lv_obj_create(main_screen);
    lv_obj_set_pos(bot_panel, 0, BOT_Y);
    lv_obj_set_size(bot_panel, SCR_W, BOT_H);
    lv_obj_set_style_bg_color(bot_panel, lv_color_hex(0x0A0A14), 0);
    lv_obj_set_style_border_width(bot_panel, 0, 0);
    lv_obj_set_style_radius(bot_panel, 0, 0);
    lv_obj_set_style_pad_all(bot_panel, 0, 0);
    lv_obj_clear_flag(bot_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bot_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bot_panel, graph_touch_cb, LV_EVENT_CLICKED, NULL);

    initGraph(bot_panel, 2, 2, SCR_W - 4, BOT_H - 4);

    lv_scr_load(main_screen);
    LOG_INFO("DISP", "Main screen initialized");
}

static void top_touch_cb(lv_event_t* e)   { (void)e; showDetailScreen(); }
static void graph_touch_cb(lv_event_t* e) { (void)e; graphNextSensor(); }
static void detail_touch_cb(lv_event_t* e){ (void)e; hideDetailScreen(); }
static void detail_timer_cb(lv_timer_t* t){ (void)t; hideDetailScreen(); }

void showDetailScreen() {
    if (detail_screen) { lv_obj_del(detail_screen); detail_screen = nullptr; }
    if (detail_timer)  { lv_timer_del(detail_timer); detail_timer = nullptr; }

    detail_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(detail_screen, lv_color_hex(0x080810), 0);
    lv_obj_add_flag(detail_screen, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(detail_screen, detail_touch_cb, LV_EVENT_CLICKED, NULL);

    int y = 4;
    char buf[64];
    lv_color_t white = lv_color_hex(0xDDDDDD);

    auto addLabel = [&](int x, int yp, const lv_font_t* f, lv_color_t c, const char* txt) {
        lv_obj_t* l = lv_label_create(detail_screen);
        lv_obj_set_pos(l, x, yp);
        lv_obj_set_style_text_font(l, f, 0);
        lv_obj_set_style_text_color(l, c, 0);
        lv_label_set_text(l, txt);
    };

    auto addRow = [&](const char* label, const char* value, lv_color_t col) {
        addLabel(8,   y, &lv_font_montserrat_14, lv_color_hex(COL_TEXT_SECONDARY), label);
        addLabel(110, y, &lv_font_montserrat_14, col, value);
        y += 18; // povečamo razmik zaradi večjega fonta
    };

    auto addSep = [&](lv_color_t col) {
        y += 4;
        lv_obj_t* s = lv_obj_create(detail_screen);
        lv_obj_set_pos(s, 0, y); lv_obj_set_size(s, SCR_W, 1);
        lv_obj_set_style_bg_color(s, col, 0);
        lv_obj_set_style_border_width(s, 0, 0); lv_obj_set_style_radius(s, 0, 0);
        y += 6;
    };

    // Header
    addLabel(8, y, &lv_font_montserrat_16, lv_color_hex(0x4DA6FF), "Podrobnosti");
    y += 22;
    addSep(lv_color_hex(0x2A2A50));

    // Senzorji
    addLabel(8, y, &lv_font_montserrat_14, lv_color_hex(COL_ACCENT_BLUE), "Senzorji");
    y += 18;

    const SensorData& sd = sensorData;

    // temp: float, veljavno če > ERR_FLOAT (-999)
    if (sd.temp > ERR_FLOAT) {
        snprintf(buf, sizeof(buf), "%.2f \xC2\xB0""C", sd.temp);
        addRow("Temperatura:", buf, lv_color_hex(0xFF8080));
    }

    // FIX: bilo sd.humidity - SensorData ima "hum", ne "humidity"
    if (sd.hum > ERR_FLOAT) {
        snprintf(buf, sizeof(buf), "%.1f %%", sd.hum);
        addRow("Vlaznost:", buf, lv_color_hex(0x4DA6FF));
    }

    // press: float
    if (sd.press > ERR_FLOAT) {
        snprintf(buf, sizeof(buf), "%.1f hPa", sd.press);
        addRow("Tlak:", buf, lv_color_hex(0xA8D8A8));
    }

    // FIX: iaq je uint16_t, primerjava z -998.0f VEDNO true.
    // Pravilno: prikaži če BME680 je prisoten in accuracy > 0, ali iaq > 0.
    if (bme680Present && sd.iaqAccuracy > 0) {
        snprintf(buf, sizeof(buf), "%u (acc:%d)", sd.iaq, sd.iaqAccuracy);
        addRow("IAQ:", buf, iaqColor((float)sd.iaq));
    }

    // FIX: staticIaq je uint16_t - ista logika
    if (bme680Present && sd.iaqAccuracy > 0) {
        snprintf(buf, sizeof(buf), "%u", sd.staticIaq);
        addRow("sIAQ:", buf, iaqColor((float)sd.staticIaq));
    }

    // eCO2: float, veljavno če > ERR_FLOAT
    if (sd.eCO2 > ERR_FLOAT) {
        snprintf(buf, sizeof(buf), "%.0f ppm", sd.eCO2);
        addRow("eCO2:", buf, lv_color_hex(0xFF8C42));
    }

    // breathVOC: float
    if (sd.breathVOC > ERR_FLOAT) {
        snprintf(buf, sizeof(buf), "%.2f ppm", sd.breathVOC);
        addRow("bVOC:", buf, lv_color_hex(0xCC88FF));
    }

    // lux: float
    if (sd.lux > ERR_FLOAT) {
        snprintf(buf, sizeof(buf), "%.0f lux %dK", sd.lux, (int)sd.cct);
        addRow("Svetloba:", buf, lv_color_hex(0xFFF176));
    }

    snprintf(buf, sizeof(buf), "%s x%d", sd.motion ? "DA" : "ne", sd.motionCount);
    addRow("Gibanje:", buf, sd.motion ? lv_color_hex(0xFF6644) : lv_color_hex(COL_TEXT_DIM));

    if (sd.bat > ERR_FLOAT && sd.bat > 2.5f) {
        int pct = (int)sd.batPct;
        snprintf(buf, sizeof(buf), "%.2fV (%d%%)", sd.bat, pct);
        addRow("Baterija:", buf,
            sd.bat > 3.7f ? lv_color_hex(COL_BAT_HIGH) :
            sd.bat > 3.3f ? lv_color_hex(COL_BAT_MID) : lv_color_hex(COL_BAT_LOW));
    }

    // Vreme
    addSep(lv_color_hex(0x2A2A50));
    addLabel(8, y, &lv_font_montserrat_14, lv_color_hex(COL_ACCENT_TEAL), "Vreme (OpenMeteo)");
    y += 18;

    if (weatherData.valid) {
        snprintf(buf, sizeof(buf), "%s (%d)", weatherCodeToStr(weatherData.weatherCode), weatherData.weatherCode);
        addRow("Stanje:", buf, lv_color_hex(0x88CCFF));
        snprintf(buf, sizeof(buf), "%.1f \xC2\xB0""C", weatherData.dewPoint);
        addRow("Rosna tocka:", buf, lv_color_hex(0x80CCEE));
        snprintf(buf, sizeof(buf), "%.1f km/h", weatherData.windSpeed);
        addRow("Veter 10m:", buf, lv_color_hex(0x80CBC4));
        snprintf(buf, sizeof(buf), "%.1f mm", weatherData.precipitation);
        addRow("Padavine:", buf, white);
        if (weatherData.rain > 0.01f) {
            snprintf(buf, sizeof(buf), "%.1f mm", weatherData.rain);
            addRow("  Dez:", buf, lv_color_hex(0x4DA6FF));
        }
        if (weatherData.snowfall > 0.01f) {
            snprintf(buf, sizeof(buf), "%.1f cm", weatherData.snowfall);
            addRow("  Sneg:", buf, lv_color_hex(0xCCEEFF));
        }
        snprintf(buf, sizeof(buf), "%d%% L:%d M:%d H:%d",
            weatherData.cloudCover, weatherData.cloudCoverLow,
            weatherData.cloudCoverMid, weatherData.cloudCoverHigh);
        addRow("Oblaki:", buf, lv_color_hex(0xB0BEC5));
        if (weatherData.soilTemp > -998.0f) {
            snprintf(buf, sizeof(buf), "%.1f \xC2\xB0""C", weatherData.soilTemp);
            addRow("Temp. tal:", buf, lv_color_hex(0xA1887F));
        }
        if (weatherData.soilMoisture > -998.0f) {
            snprintf(buf, sizeof(buf), "%.1f %%", weatherData.soilMoisture * 100.0f);
            addRow("Vlaznost tal:", buf, lv_color_hex(0x64B5F6));
        }
    } else {
        addRow("Vreme:", "N/A", lv_color_hex(COL_TEXT_DIM));
    }

    // Sistem
    addSep(lv_color_hex(0x2A2A50));
    addLabel(8, y, &lv_font_montserrat_14, lv_color_hex(COL_OK), "Sistem");
    y += 18;

    addRow("ID:", settings.unitId, white);
    if (WiFi.status() == WL_CONNECTED)
        snprintf(buf, sizeof(buf), "%s (%d dBm)", WiFi.SSID().c_str(), WiFi.RSSI());
    else
        snprintf(buf, sizeof(buf), "Ni povezave");
    addRow("WiFi:", buf, WiFi.status() == WL_CONNECTED ? lv_color_hex(0x44FF88) : lv_color_hex(0xFF4444));
    addRow("IP:", WiFi.localIP().toString().c_str(), white);

    unsigned long up = millis() / 1000;
    snprintf(buf, sizeof(buf), "%lud %02lu:%02lu", up/86400, (up%86400)/3600, (up%3600)/60);
    addRow("Uptime:", buf, white);
    snprintf(buf, sizeof(buf), "%u B", ESP.getFreeHeap());
    addRow("Heap:", buf, white);
    snprintf(buf, sizeof(buf), "0x%02X", sensorData.err);
    addRow("Err:", buf, sensorData.err ? lv_color_hex(0xFF4444) : lv_color_hex(0x44AA44));

    // Footer
    lv_obj_t* footer = lv_label_create(detail_screen);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_text_font(footer, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(footer, lv_color_hex(COL_TEXT_DIM), 0);
    lv_label_set_text(footer, "tap za nazaj - auto " LV_SYMBOL_CLOSE);

    lv_scr_load_anim(detail_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);

    detail_timer = lv_timer_create(detail_timer_cb, DETAIL_TIMEOUT_SEC * 1000, NULL);
    lv_timer_set_repeat_count(detail_timer, 1);
}

void hideDetailScreen() {
    if (detail_timer) { lv_timer_del(detail_timer); detail_timer = nullptr; }
    lv_scr_load_anim(main_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
    if (detail_screen) {
        lv_timer_t* del_t = lv_timer_create([](lv_timer_t* t){
            if (detail_screen) { lv_obj_del(detail_screen); detail_screen = nullptr; }
            lv_timer_del(t);
        }, 250, NULL);
        lv_timer_set_repeat_count(del_t, 1);
    }
}

static unsigned long lastUiUpdate = 0;

void updateUI() {
    unsigned long now = millis();
    if (now - lastUiUpdate < 1000) { lv_timer_handler(); return; }
    lastUiUpdate = now;

    time_t t = time(nullptr);
    struct tm* ti = localtime(&t);
    char tbuf[10], dbuf[12];
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S", ti);
    strftime(dbuf, sizeof(dbuf), "%d.%m.%Y", ti);
    lv_label_set_text(lbl_time, tbuf);
    lv_label_set_text(lbl_date, dbuf);

    bool conn = (WiFi.status() == WL_CONNECTED);
    int rssi = conn ? WiFi.RSSI() : -100;
    lv_label_set_text(lbl_wifi, conn ? LV_SYMBOL_WIFI : LV_SYMBOL_WIFI " X");
    lv_obj_set_style_text_color(lbl_wifi, wifiColor(rssi, conn), 0);

    if (weatherData.valid) {
        lv_label_set_text(lbl_weather, weatherCodeToIcon(weatherData.weatherCode));
    }

    const SensorData& sd = sensorData;
    char buf[20];

    // temp: float, veljavno če > ERR_FLOAT
    if (sd.temp > ERR_FLOAT) {
        snprintf(buf, sizeof(buf), "%.1f\xC2\xB0\x43", sd.temp);
        lv_label_set_text(lbl_temp, buf);
    }

    // FIX: bilo sd.humidity - pravilno je sd.hum
    if (sd.hum > ERR_FLOAT) {
        snprintf(buf, sizeof(buf), "%.0f%%", sd.hum);
        lv_label_set_text(lbl_hum, buf);
    }

    // press: float
    if (sd.press > ERR_FLOAT) {
        snprintf(buf, sizeof(buf), "%.0f hPa", sd.press);
        lv_label_set_text(lbl_press, buf);
    }

    // FIX: iaq je uint16_t. Prikažemo samo ob veljavnih podatkih (bme680Present && accuracy > 0)
    if (bme680Present && sd.iaqAccuracy > 0) {
        snprintf(buf, sizeof(buf), "%u", sd.iaq);
        lv_label_set_text(lbl_iaq, buf);
        lv_obj_set_style_text_color(lbl_iaq, iaqColor((float)sd.iaq), 0);
    }

    // lux: float
    if (sd.lux > ERR_FLOAT) {
        if (sd.lux >= 1000) snprintf(buf, sizeof(buf), "%.1fk", sd.lux/1000.0f);
        else snprintf(buf, sizeof(buf), "%.0f", sd.lux);
        lv_label_set_text(lbl_lux, buf);
    }

    lv_obj_set_style_text_color(lbl_pir,
        sd.motion ? lv_color_hex(0xFF6644) : lv_color_hex(COL_TEXT_SECONDARY), 0);

    graphRefresh();
    lv_timer_handler();
}
