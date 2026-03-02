// LVGL_Driver.cpp - Waveshare ESP32-S3-Touch-LCD-2
// 240x320, s touch — PSRAM buffer + CST816D touch driver
//
// POPRAVEK (2026-03-02): DODAN touch input driver za LVGL
//   - Touch_Init(): inicializira CST816D prek I2C (SDA=IO48, SCL=IO47)
//   - Touch_Read(): bere koordinate in stanje dotika
//   - Lvgl_Init(): registrira touch input device pri LVGL (lv_indev_drv_t)
//
// CST816D protokol:
//   - I2C naslov: 0x15
//   - Register 0x01: gesture
//   - Register 0x02: finger count
//   - Register 0x03: X high nibble + event
//   - Register 0x04: X low
//   - Register 0x05: Y high nibble
//   - Register 0x06: Y low
//
// Brez touch driverja LVGL ne prejme dotikov → event callback-i ne delujejo!

#include "LVGL_Driver.h"
#include <esp_heap_caps.h>
#include <Wire.h>

// Buffer višina iz config.h (LVGL_DRAW_BUF_HEIGHT = 60)
#ifndef LVGL_DRAW_BUF_HEIGHT
  #define LVGL_DRAW_BUF_HEIGHT 60
#endif

// ============================================================
// TOUCH - CST816D
// ============================================================

// Inicializacija CST816D touch čipa
void Touch_Init(void) {
    Serial.println("[DEBUG] Touch_Init: starting");

    // CST816D reset sequence:
    // TP_RST je vezan na LCD_RST (IO0) - LCD_Init() ga je že resetiral.
    // CST816D potrebuje vsaj 50ms po RST HIGH preden odgovori na I2C.
    // LCD_Init() traja ~300ms skupaj, ampak dodamo še varnostni delay.
    delay(100);

    // INT pin - CST816D v privzetem modu drži INT LOW ko ni dotika.
    // Nastavi kot INPUT (brez pull-up - čip ima interni pull-up na INT).
    pinMode(TOUCH_INT_PIN, INPUT);
    delay(10);

    // I2C bus 0: Touch + IMU (SDA=IO48, SCL=IO47)
    // POZOR: Začni s 100kHz - CST816D je zanesljiv pri 100kHz.
    // 400kHz povzroča timeoutte takoj po resetu.
    Wire.begin(TOUCH_SDA_PIN, TOUCH_SCL_PIN, 100000);
    delay(50);  // čakaj da se I2C bus stabilizira

    // Preveri prisotnost CST816D (do 3 poskuse)
    bool found = false;
    for (int attempt = 0; attempt < 3; attempt++) {
        Wire.beginTransmission(CST816D_I2C_ADDR);
        uint8_t err = Wire.endTransmission();
        if (err == 0) {
            found = true;
            break;
        }
        Serial.printf("[DEBUG] Touch_Init: attempt %d failed (err=%d), retrying...\n",
                      attempt + 1, err);
        delay(50);
    }

    if (!found) {
        Serial.printf("[WARN] Touch_Init: CST816D not found at 0x%02X after 3 attempts\n",
                      CST816D_I2C_ADDR);
    } else {
        Serial.printf("[DEBUG] Touch_Init: CST816D found at 0x%02X\n", CST816D_I2C_ADDR);

        // Preberi chip ID (register 0xA7) za verifikacijo
        Wire.beginTransmission(CST816D_I2C_ADDR);
        Wire.write(0xA7);
        if (Wire.endTransmission(false) == 0) {
            Wire.requestFrom((uint8_t)CST816D_I2C_ADDR, (uint8_t)1);
            if (Wire.available()) {
                uint8_t chipId = Wire.read();
                Serial.printf("[DEBUG] Touch_Init: CST816D chip ID = 0x%02X\n", chipId);
            }
        }

        // Nastavi IRQ mode: register 0xFA - kontinuirni mode (0x01)
        // Da Touch_Read() deluje v polling modu brez interrupt-a
        Wire.beginTransmission(CST816D_I2C_ADDR);
        Wire.write(0xFA);  // IrqCtl register
        Wire.write(0x71);  // EnTest | EnChange | EnMotion | LongPressEn  → redno poroča
        Wire.endTransmission();
        delay(10);
    }

    Serial.println("[DEBUG] Touch_Init: done");
}

// Branje touch koordinat za LVGL
// Vrne true če je dotik aktiven, false sicer
void Touch_Read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    (void)drv;

    // Branje 6 registrov od 0x01 naprej
    Wire.beginTransmission(CST816D_I2C_ADDR);
    Wire.write(0x01);  // začni pri registru gesture
    if (Wire.endTransmission(false) != 0) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    uint8_t buf[6] = {0};
    Wire.requestFrom((uint8_t)CST816D_I2C_ADDR, (uint8_t)6);
    int idx = 0;
    while (Wire.available() && idx < 6) {
        buf[idx++] = Wire.read();
    }

    if (idx < 6) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    // buf[0] = gesture (0x00=none, 0x01=up, 0x02=down, 0x03=left, 0x04=right, 0x05=click)
    // buf[1] = finger count
    uint8_t fingers = buf[1] & 0x0F;

    if (fingers == 0) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    // buf[2] = event (bits 7:6) + X high (bits 3:0)
    // buf[3] = X low
    // buf[4] = Y high (bits 3:0)
    // buf[5] = Y low
    uint16_t x = ((uint16_t)(buf[2] & 0x0F) << 8) | buf[3];
    uint16_t y = ((uint16_t)(buf[4] & 0x0F) << 8) | buf[5];

    // Omeji na zaslon (240x320)
    if (x >= LCD_WIDTH)  x = LCD_WIDTH  - 1;
    if (y >= LCD_HEIGHT) y = LCD_HEIGHT - 1;

    data->point.x = (lv_coord_t)x;
    data->point.y = (lv_coord_t)y;
    data->state   = LV_INDEV_STATE_PR;
}

// LVGL flush callback — posreduje LVGL buffer na LCD
void Lvgl_Display_LCD(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p) {
    LCD_addWindow(area->x1, area->y1, area->x2, area->y2, (uint16_t*)&color_p->full);
    lv_disp_flush_ready(disp_drv);
}

// ============================================================
// LVGL inicializacija + registracija display in touch driverja
// ============================================================
void Lvgl_Init(void) {
    Serial.println("[DEBUG] Lvgl_Init: calling lv_init()");
    lv_init();
    Serial.println("[DEBUG] Lvgl_Init: lv_init() done");

    // --- DISPLAY DRIVER ---

    size_t bufSize = LCD_WIDTH * LVGL_DRAW_BUF_HEIGHT * sizeof(lv_color_t);

    Serial.println("[DEBUG] Lvgl_Init: init draw buffer with PSRAM");

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

    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, LCD_WIDTH * LVGL_DRAW_BUF_HEIGHT);

    Serial.println("[DEBUG] Lvgl_Init: registering display driver");
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = LVGL_WIDTH;
    disp_drv.ver_res  = LVGL_HEIGHT;
    disp_drv.flush_cb = Lvgl_Display_LCD;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);
    Serial.println("[DEBUG] Lvgl_Init: display driver registered");

    // --- TOUCH INPUT DRIVER ---
    // KLJUČNO: Brez tega LVGL ne prejme nobenih touch eventov!

    Serial.println("[DEBUG] Lvgl_Init: initializing touch (CST816D)");
    Touch_Init();

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;  // Touch = pointer tip
    indev_drv.read_cb = Touch_Read;             // Callback za branje koordinat
    lv_indev_drv_register(&indev_drv);
    Serial.println("[DEBUG] Lvgl_Init: touch driver registered");

    Serial.println("Lvgl_Init OK (240x320, PSRAM buffer, CST816D touch)");
}
