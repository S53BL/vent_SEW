// Display_ST7789.h - Waveshare ESP32-S3-Touch-LCD-2
// ST7789T3, 240x320, SPI, s touch
// GPIO pini iz config.h

#pragma once
#include <Arduino.h>
#include <SPI.h>

// ============================================================
// Ekran
// ============================================================
#define LCD_WIDTH   240
#define LCD_HEIGHT  320

// ============================================================
// GPIO pini — Waveshare ESP32-S3-Touch-LCD-2 (iz config.h)
// ============================================================
#define EXAMPLE_PIN_NUM_MISO  -1   // ni MISO
#define EXAMPLE_PIN_NUM_MOSI  38   // LCD_MOSI_PIN
#define EXAMPLE_PIN_NUM_SCLK  39   // LCD_SCLK_PIN
#define EXAMPLE_PIN_NUM_LCD_CS  45 // LCD_CS_PIN
#define EXAMPLE_PIN_NUM_LCD_DC  42 // LCD_DC_PIN
#define EXAMPLE_PIN_NUM_LCD_RST 0  // LCD_RST_PIN
#define LCD_Backlight_PIN       1  // LCD_BL_PIN

// ============================================================
// SPI hitrost
// ============================================================
#define SPIFreq  40000000UL   // 40MHz

// ============================================================
// Backlight PWM
// ============================================================
#define LEDC_CHANNEL   0
#define LEDC_FREQ      5000    // iz config.h LEDC_BL_FREQ
#define LEDC_RES       10      // 10-bit = 0-1023
#define Backlight_MAX  100

// ============================================================
// Zunanje spremenljivke
// ============================================================
extern uint8_t LCD_Backlight;

// ============================================================
// Funkcije
// ============================================================
void LCD_Init(void);
void LCD_SetCursor(uint16_t Xstart, uint16_t Ystart, uint16_t Xend, uint16_t Yend);
void LCD_addWindow(uint16_t Xstart, uint16_t Ystart, uint16_t Xend, uint16_t Yend, uint16_t* color);

void Backlight_Init(void);
void Set_Backlight(uint8_t Light);