// colours.h - Barvne konstante za vent_SEW (LVGL zaslon)
// Plošča: Waveshare ESP32-S3-Touch-LCD-2, ST7789T3 240×320
//
// SEW zaslon prikazuje lokalne podatke (temp, hum, IAQ, kamera, baterija).
// Barvna shema: temno ozadje z barvnimi poudarki po pomenu meritve.

#ifndef COLOURS_H
#define COLOURS_H

#include <stdint.h>

// --- Ozadje ---
#define COL_BG              0x121212   // Temno ozadje
#define COL_CARD_BG         0x1E1E2A   // Kartica
#define COL_HEADER_BG       0x1A1A2E   // Header bar

// --- Tekst ---
#define COL_TEXT_PRIMARY    0xFFFFFF   // Beli tekst - vrednosti
#define COL_TEXT_SECONDARY  0xAAAAAA   // Sivi tekst - oznake
#define COL_TEXT_DIM        0x555555   // Zelo svetlo - neaktivno

// --- Akcenti ---
#define COL_ACCENT_BLUE     0x4DA6FF   // Glavni poudarek
#define COL_ACCENT_TEAL     0x26C6DA   // Sekundarni poudarek

// --- Stanje ---
#define COL_OK              0x4CAF50   // Zelena - OK
#define COL_WARN            0xFFC107   // Rumena - opozorilo
#define COL_ERR             0xF44336   // Rdeča - napaka
#define COL_INACTIVE        0x444455   // Sivo - neaktivno

// --- IAQ barvna lestvica (IAQ 0-500) ---
#define COL_IAQ_EXCELLENT   0x00E676   // 0-50    zelena svetla
#define COL_IAQ_GOOD        0x69F0AE   // 51-100  zelena
#define COL_IAQ_MODERATE    0xFFFF00   // 101-150 rumena
#define COL_IAQ_POOR        0xFF9100   // 151-200 oranžna
#define COL_IAQ_BAD         0xFF3D00   // 201-300 rdeče-oranžna
#define COL_IAQ_HAZARDOUS   0xD50000   // 301+    temno rdeča

// --- Kamera ---
#define COL_CAM_STREAMING   0x00C853   // Zelena - stream aktiven
#define COL_CAM_RECORDING   0xF44336   // Rdeča  - snemanje aktivno
#define COL_CAM_IDLE        0x444455   // Sivo   - mirovanje

// --- Baterija ---
#define COL_BAT_HIGH        0x4CAF50   // >60%
#define COL_BAT_MID         0xFFC107   // 30-60%
#define COL_BAT_LOW         0xF44336   // <30%

// --- Grafikon ozadje ---
#define COL_CHART_BG        0x0D0D0D
#define COL_CHART_GRID      0x2A2A2A
#define COL_CHART_TEMP      0xFF7043   // oranžna - temperatura
#define COL_CHART_HUM       0x42A5F5   // modra   - vlažnost
#define COL_CHART_IAQ       0xAB47BC   // vijolična - IAQ

// --- Radiusi ---
#define CARD_RADIUS         10
#define BADGE_RADIUS         4

// --- Fonti (definirani v lv_conf.h / platformio.ini) ---
#define FONT_10  &lv_font_montserrat_10
#define FONT_12  &lv_font_montserrat_12
#define FONT_14  &lv_font_montserrat_14
#define FONT_16  &lv_font_montserrat_16
#define FONT_20  &lv_font_montserrat_20
#define FONT_24  &lv_font_montserrat_24

#endif // COLOURS_H
