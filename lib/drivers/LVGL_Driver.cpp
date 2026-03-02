// LVGL_Driver.cpp - Waveshare ESP32-S3-Touch-LCD-2
// 240x320, s touch — uporabi PSRAM za buffer
//
// POPRAVEK (2026-03-02): KRITIČNO - NULL draw buffer povzročal StoreProhibited crash
//
// NAPAKA (bilo prej):
//   lv_disp_draw_buf_init(&draw_buf, NULL, NULL, ...);
//   buf1 in buf2 sta bila NULL → LVGL je pisal na naslov 0x00000000 → crash
//   Komentar "PSRAM buffer" je bil napačen - ps_malloc() se ni nikoli klical!
//
// POPRAVEK (zdaj):
//   buf1 = (lv_color_t*) heap_caps_malloc(BUF_SIZE, MALLOC_CAP_SPIRAM)
//   buf2 = (lv_color_t*) heap_caps_malloc(BUF_SIZE, MALLOC_CAP_SPIRAM)  // double buffer
//   BUF_SIZE = LCD_WIDTH * LVGL_DRAW_BUF_HEIGHT * sizeof(lv_color_t)
//            = 240 * 60 * 2 = 28.800 bytes vsak buffer
//   Skupaj: 57.600 bytes v PSRAM (od 8MB razpoložljivih)
//
// Če PSRAM ni dostopen (fallback): heap_caps_malloc(MALLOC_CAP_DEFAULT)
// Ob OOM: Serial.println + while(1) → jasno sporočilo namesto skrivnostnega crash

#include "LVGL_Driver.h"
#include <esp_heap_caps.h>

// Buffer višina iz config.h (LVGL_DRAW_BUF_HEIGHT = 60)
// Če config.h ni vključen prek LVGL_Driver.h, definiraj fallback
#ifndef LVGL_DRAW_BUF_HEIGHT
  #define LVGL_DRAW_BUF_HEIGHT 60
#endif

// LVGL flush callback — posreduje LVGL buffer na LCD
void Lvgl_Display_LCD(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p) {
    LCD_addWindow(area->x1, area->y1, area->x2, area->y2, (uint16_t*)&color_p->full);
    lv_disp_flush_ready(disp_drv);
}

// Inicializacija LVGL + display driver
// Uporabi PSRAM za draw buffer
void Lvgl_Init(void) {
    Serial.println("[DEBUG] Lvgl_Init: calling lv_init()");
    lv_init();
    Serial.println("[DEBUG] Lvgl_Init: lv_init() done");

    // Izračun velikosti bufferja
    // LCD_WIDTH=240, LVGL_DRAW_BUF_HEIGHT=60 → 28.800 bytes na buffer
    size_t bufSize = LCD_WIDTH * LVGL_DRAW_BUF_HEIGHT * sizeof(lv_color_t);

    Serial.println("[DEBUG] Lvgl_Init: init draw buffer with PSRAM");

    // POPRAVEK: Alociraj buf1 in buf2 v PSRAM
    // NAPAKA prej: NULL, NULL → pisanje na naslov 0x00000000 → StoreProhibited crash
    lv_color_t* buf1 = (lv_color_t*) heap_caps_malloc(bufSize, MALLOC_CAP_SPIRAM);
    lv_color_t* buf2 = (lv_color_t*) heap_caps_malloc(bufSize, MALLOC_CAP_SPIRAM);

    // Fallback na internal heap če PSRAM ni dostopen
    if (!buf1) {
        Serial.println("[WARN] Lvgl_Init: PSRAM buf1 failed, trying internal heap");
        buf1 = (lv_color_t*) heap_caps_malloc(bufSize, MALLOC_CAP_DEFAULT);
    }
    if (!buf2) {
        Serial.println("[WARN] Lvgl_Init: PSRAM buf2 failed, trying internal heap");
        buf2 = (lv_color_t*) heap_caps_malloc(bufSize, MALLOC_CAP_DEFAULT);
    }

    // Ob popolnem OOM: jasno sporočilo (ne skrivnosten crash)
    if (!buf1) {
        Serial.printf("[ERROR] Lvgl_Init: buf1 alloc FAILED (%u bytes)! Halting.\n", bufSize);
        while (1) delay(1000);
    }
    if (!buf2) {
        Serial.printf("[WARN] Lvgl_Init: buf2 alloc failed, using single buffer\n");
        // Single buffer je OK - samo počasnejše izrisovanje
    }

    Serial.printf("[DEBUG] Lvgl_Init: buf1=%p buf2=%p size=%u bytes each\n",
                  (void*)buf1, (void*)buf2, bufSize);

    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, LCD_WIDTH * LVGL_DRAW_BUF_HEIGHT);

    Serial.println("[DEBUG] Lvgl_Init: registering display driver");
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res     = LVGL_WIDTH;
    disp_drv.ver_res     = LVGL_HEIGHT;
    disp_drv.flush_cb    = Lvgl_Display_LCD;
    disp_drv.draw_buf    = &draw_buf;
    lv_disp_drv_register(&disp_drv);
    Serial.println("[DEBUG] Lvgl_Init: display driver registered");

    Serial.println("Lvgl_Init OK (240x320, PSRAM buffer)");
}
