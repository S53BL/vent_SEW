// LVGL_Driver.h - Waveshare ESP32-S3-Touch-LCD-2
// 240x320, s touch

#pragma once
#include <lvgl.h>
#include "Display_ST7789.h"

// Buffer — uporabi LVGL draw buf iz lv_conf.h
#define LVGL_WIDTH   LCD_WIDTH
#define LVGL_HEIGHT  LCD_HEIGHT

#define EXAMPLE_LVGL_TICK_PERIOD_MS  2

void Lvgl_Init(void);
void Lvgl_Display_LCD(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p);
void example_increase_lvgl_tick(void *arg);