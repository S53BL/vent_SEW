// LVGL_Driver.h - Waveshare ESP32-S3-Touch-LCD-2
// 240x320, s touch CST816D (I2C: SDA=IO48, SCL=IO47, INT=IO46)

#pragma once
#include <lvgl.h>
#include "Display_ST7789.h"

// Buffer — uporabi LVGL draw buf iz lv_conf.h
#define LVGL_WIDTH   LCD_WIDTH
#define LVGL_HEIGHT  LCD_HEIGHT

// Touch I2C naslovi in pini (morajo se ujemati z config.h)
#define CST816D_I2C_ADDR  0x15
#define TOUCH_SDA_PIN     48
#define TOUCH_SCL_PIN     47
#define TOUCH_INT_PIN     46

// LVGL driver funkcije
void Lvgl_Init(void);
void Lvgl_Display_LCD(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p);

// Touch funkcije
void  Touch_Init(void);
void  Touch_Read(lv_indev_drv_t *drv, lv_indev_data_t *data);
