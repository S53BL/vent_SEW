// LVGL_Driver.cpp - Waveshare ESP32-S3-Touch-LCD-2
// 240x320, s touch — Display + Touch driver za LVGL
//
// POPRAVEK (2026-03-02):
//   - FIX kritično: NULL draw buffer povzročal StoreProhibited crash → PSRAM alloc
//   - DODANO: CST816D touch inicializacija (Wire0: SDA=IO48, SCL=IO47, INT=IO46)
//   - DODANO: lv_indev_drv_t registracija → LVGL dobi touch events
//
// Brez touch driverja LVGL ne more prejemati dotikov, čeprav je hardware OK.

#include "LVGL_Driver.h"
#include <esp_heap_caps.h>
#include <Wire.h>

// Buffer višina iz config.h (LVGL_DRAW_BUF_HEIGHT = 60)
#ifndef LVGL_DRAW_BUF_HEIGHT
  #define LVGL_DRAW_BUF_HEIGHT 60
#endif

// CST816D I2C naslov in registri
#define CST816D_I2C_ADDR    0x15
#define CST816D_REG_GESTURE 0x01
#define CST816D_REG_FINGER  0x02
#define CST816D_REG_X_HIGH  0x03
#define CST816D_REG_X_LOW   0x04
#define CST816D_REG_Y_HIGH  0x05
#define CST816D_REG_Y_LOW   0x06
#define CST816D_REG_CHIP_ID 0xA7

// I2C bus za touch (IO47=SCL, IO48=SDA)
static TwoWire touchWire = TwoWire(0);

// Zadnje znane koordinate (za LVGL)
static int16_t touch_last_x = 0;
static int16_t touch_last_y = 0;
static bool    touch_pressed = false;

// ============================================================
// CST816D: init
// ============================================================
static bool cst816d_init(void) {
    touchWire.begin(TP_SDA_PIN, TP_SCL_PIN, 400000);

    // Preveri chip ID
    touchWire.beginTransmission(CST816D_I2C_ADDR);
    touchWire.write(CST816D_REG_CHIP_ID);
    if (touchWire.endTransmission(false) != 0) {
        Serial.println("[TOUCH:ERROR] CST816D not found on I2C (0x15)");
        return false;
    }
    touchWire.requestFrom((uint8_t)CST816D_I2C_ADDR, (uint8_t)1);
    if (touchWire.available()) {
        uint8_t id = touchWire.read();
        Serial.printf("[TOUCH:INFO] CST816D chip ID: 0x%02X\n", id);
    }

    // Konfiguriraj INT pin kot vhod
    pinMode(TP_INT_PIN, INPUT);

    Serial.println("[TOUCH:INFO] CST816D initialized (SDA=48, SCL=47, INT=46)");
    return true;
}

// ============================================================
// CST816D: preberi touch točko
// ============================================================
static bool cst816d_read(int16_t* x, int16_t* y) {
    // Preberi 7 registrov od 0x01 naprej
    touchWire.beginTransmission(CST816D_I2C_ADDR);
    touchWire.write(CST816D_REG_GESTURE);
    if (touchWire.endTransmission(false) != 0) return false;

    uint8_t data[6];
    uint8_t received = touchWire.requestFrom((uint8_t)CST816D_I2C_ADDR, (uint8_t)6);
    if (received < 6) return false;

    for (int i = 0; i < 6; i++) data[i] = touchWire.read();

    // data[1] = finger count, data[2..5] = X high/low, Y high/low
    uint8_t fingers = data[1] & 0x0F;
    if (fingers == 0) return false;

    uint16_t raw_x = ((data[2] & 0x0F) << 8) | data[3];
    uint16_t raw_y = ((data[4] & 0x0F) << 8) | data[5];

    // Zaslonska orientacija: portrait 240x320, brez rotacije
    *x = (int16_t)raw_x;
    *y = (int16_t)raw_y;

    // Mejne vrednosti
    if (*x < 0) *x = 0;
    if (*x >= LCD_WIDTH) *x = LCD_WIDTH - 1;
    if (*y < 0) *y = 0;
    if (*y >= LCD_HEIGHT) *y = LCD_HEIGHT - 1;

    return true;
}

// ============================================================
// LVGL touch callback
// ============================================================
static void lvgl_touch_cb(lv_indev_drv_t* drv, lv_indev_data_t* data) {
    (void)drv;
    int16_t x = 0, y = 0;

    if (cst816d_read(&x, &y)) {
        touch_last_x = x;
        touch_last_y = y;
        touch_pressed = true;
    } else {
        touch_pressed = false;
    }

    data->point.x = touch_last_x;
    data->point.y = touch_last_y;
    data->state   = touch_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// ============================================================
// LVGL flush callback — posreduje LVGL buffer na LCD
// ============================================================
void Lvgl_Display_LCD(lv_disp_drv_t* disp_drv, const lv_area_t* area, lv_color_t* color_p) {
    LCD_addWindow(area->x1, area->y1, area->x2, area->y2, (uint16_t*)&color_p->full);
    lv_disp_flush_ready(disp_drv);
}

// ============================================================
// Inicializacija LVGL + display driver + touch driver
// ============================================================
void Lvgl_Init(void) {
    Serial.println("[DEBUG] Lvgl_Init: calling lv_init()");
    lv_init();
    Serial.println("[DEBUG] Lvgl_Init: lv_init() done");

    // Izračun velikosti bufferja
    size_t bufSize = LCD_WIDTH * LVGL_DRAW_BUF_HEIGHT * sizeof(lv_color_t);

    // Alociraj buf1 in buf2 v PSRAM
    lv_color_t* buf1 = (lv_color_t*) heap_caps_malloc(bufSize, MALLOC_CAP_SPIRAM);
    lv_color_t* buf2 = (lv_color_t*) heap_caps_malloc(bufSize, MALLOC_CAP_SPIRAM);

    if (!buf1) {
        Serial.println("[WARN] Lvgl_Init: PSRAM buf1 failed, trying internal heap");
        buf1 = (lv_color_t*) heap_caps_malloc(bufSize, MALLOC_CAP_DEFAULT);
    }
    if (!buf2) {
        Serial.println("[WARN] Lvgl_Init: PSRAM buf2 failed, trying internal heap");
        buf2 = (lv_color_t*) heap_caps_malloc(bufSize, MALLOC_CAP_DEFAULT);
    }

    if (!buf1) {
        Serial.printf("[ERROR] Lvgl_Init: buf1 alloc FAILED (%u bytes)! Halting.\n", bufSize);
        while (1) delay(1000);
    }
    if (!buf2) {
        Serial.printf("[WARN] Lvgl_Init: buf2 alloc failed, using single buffer\n");
    }

    Serial.printf("[DEBUG] Lvgl_Init: buf1=%p buf2=%p size=%u bytes each\n",
                  (void*)buf1, (void*)buf2, bufSize);

    // --- Display driver ---
    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, LCD_WIDTH * LVGL_DRAW_BUF_HEIGHT);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = LVGL_WIDTH;
    disp_drv.ver_res  = LVGL_HEIGHT;
    disp_drv.flush_cb = Lvgl_Display_LCD;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);
    Serial.println("[DEBUG] Lvgl_Init: display driver registered");

    // --- Touch driver (CST816D) ---
    bool touchOK = cst816d_init();
    if (touchOK) {
        static lv_indev_drv_t indev_drv;
        lv_indev_drv_init(&indev_drv);
        indev_drv.type    = LV_INDEV_TYPE_POINTER;
        indev_drv.read_cb = lvgl_touch_cb;
        lv_indev_drv_register(&indev_drv);
        Serial.println("[TOUCH:INFO] LVGL touch input driver registered");
    } else {
        Serial.println("[TOUCH:WARN] Touch driver NOT registered - touch will not work");
    }

    Serial.println("Lvgl_Init OK (240x320, PSRAM buffer, CST816D touch)");
}
