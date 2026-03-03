// disp.h - Display module for vent_SEW
// Zaslon: ST7789T3, 240x320, portrait
//
// Layout:
//   [0..129]   ZGORNJI DEL - senzorji + ura/datum + WiFi
//   [130..319] SPODNJI DEL - graf (LVGL lv_chart)
//
// Touch:
//   tap zgornji del -> detail screen (senzorji + OpenMeteo + sistem)
//   tap spodnji del -> naslednji senzor (cycle sensor)
//
#pragma once
#include <Arduino.h>
#include <lvgl.h>

#define SCR_W           240
#define SCR_H           320
#define TOP_H           130
#define BOT_H           190
#define BOT_Y           130

#define DETAIL_TIMEOUT_SEC  15

void initDisplay();
void updateUI();
void showDetailScreen();
void hideDetailScreen();
