// LVGL_Driver.cpp - Waveshare ESP32-S3-Touch-LCD-2
// 240x320, s touch — uporabi PSRAM za buffer

#include "LVGL_Driver.h"
#include "lvgl_psram.h"

// LVGL flush callback — posreduje LVGL buffer na LCD
void Lvgl_Display_LCD(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p) {
    LCD_addWindow(area->x1, area->y1, area->x2, area->y2, (uint16_t*)&color_p->full);
    lv_disp_flush_ready(disp_drv);
}

// LVGL tick — klic iz esp_timer vsake 2ms
void example_increase_lvgl_tick(void *arg) {
    lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

// Inicializacija LVGL + display driver
// Uporabi PSRAM za draw buffer
void Lvgl_Init(void) {
    Serial.println("[DEBUG] Lvgl_Init: calling lv_init()");
    lv_init();
    Serial.println("[DEBUG] Lvgl_Init: lv_init() done");

    Serial.println("[DEBUG] Lvgl_Init: init draw buffer with PSRAM");
    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, NULL, NULL, LV_LAYER_SIMPLE_BUF_SIZE / sizeof(lv_color_t));

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res     = LVGL_WIDTH;
    disp_drv.ver_res     = LVGL_HEIGHT;
    disp_drv.flush_cb    = Lvgl_Display_LCD;
    disp_drv.draw_buf    = &draw_buf;
    Serial.println("[DEBUG] Lvgl_Init: registering display driver");
    lv_disp_drv_register(&disp_drv);
    Serial.println("[DEBUG] Lvgl_Init: display driver registered");

    // LVGL tick timer — esp_timer vsake 2ms
    const esp_timer_create_args_t tick_args = {
        .callback = &example_increase_lvgl_tick,
        .name     = "lvgl_tick"
    };
    esp_timer_handle_t tick_timer = NULL;
    Serial.println("[DEBUG] Lvgl_Init: creating timer");
    esp_timer_create(&tick_args, &tick_timer);
    esp_timer_start_periodic(tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000);
    Serial.println("[DEBUG] Lvgl_Init: timer started");

    Serial.println("Lvgl_Init OK (240x320, PSRAM buffer)");
}