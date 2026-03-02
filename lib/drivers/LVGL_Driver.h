// LVGL_Driver.h - Waveshare ESP32-S3-Touch-LCD-2
// 240x320, s touch
//
// DODANO (2026-03-02): CST816D touch inicializacija + lv_indev_drv_t registracija

#pragma once
#include <lvgl.h>
#include "Display_ST7789.h"
#include "config.h"   // TP_SDA_PIN, TP_SCL_PIN, TP_INT_PIN, LCD_WIDTH, LCD_HEIGHT

// Buffer — uporabi LVGL draw buf iz lv_conf.h
#define LVGL_WIDTH   LCD_WIDTH
#define LVGL_HEIGHT  LCD_HEIGHT

// Display driver
void Lvgl_Init(void);
void Lvgl_Display_LCD(lv_disp_drv_t* disp_drv, const lv_area_t* area, lv_color_t* color_p);
