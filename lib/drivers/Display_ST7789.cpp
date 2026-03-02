// Display_ST7789.cpp - Waveshare ESP32-S3-Touch-LCD-2
// ST7789T3, 240x320, SPI, s touch

#include "Display_ST7789.h"

SPIClass LCDspi(FSPI);
uint8_t LCD_Backlight = 80;

// ============================================================
// SPI pomožne funkcije
// ============================================================
static void SPI_Init() {
    LCDspi.begin(EXAMPLE_PIN_NUM_SCLK, EXAMPLE_PIN_NUM_MISO, EXAMPLE_PIN_NUM_MOSI);
}

static void LCD_WriteCommand(uint8_t cmd) {
    LCDspi.beginTransaction(SPISettings(SPIFreq, MSBFIRST, SPI_MODE0));
    digitalWrite(EXAMPLE_PIN_NUM_LCD_CS, LOW);
    digitalWrite(EXAMPLE_PIN_NUM_LCD_DC, LOW);
    LCDspi.transfer(cmd);
    digitalWrite(EXAMPLE_PIN_NUM_LCD_CS, HIGH);
    LCDspi.endTransaction();
}

static void LCD_WriteData(uint8_t data) {
    LCDspi.beginTransaction(SPISettings(SPIFreq, MSBFIRST, SPI_MODE0));
    digitalWrite(EXAMPLE_PIN_NUM_LCD_CS, LOW);
    digitalWrite(EXAMPLE_PIN_NUM_LCD_DC, HIGH);
    LCDspi.transfer(data);
    digitalWrite(EXAMPLE_PIN_NUM_LCD_CS, HIGH);
    LCDspi.endTransaction();
}

static void LCD_WriteData_nbyte(uint8_t* data, uint8_t* dummy, uint32_t len) {
    LCDspi.beginTransaction(SPISettings(SPIFreq, MSBFIRST, SPI_MODE0));
    digitalWrite(EXAMPLE_PIN_NUM_LCD_CS, LOW);
    digitalWrite(EXAMPLE_PIN_NUM_LCD_DC, HIGH);
    LCDspi.transferBytes(data, dummy, len);
    digitalWrite(EXAMPLE_PIN_NUM_LCD_CS, HIGH);
    LCDspi.endTransaction();
}

static void LCD_Reset() {
    digitalWrite(EXAMPLE_PIN_NUM_LCD_RST, HIGH);
    delay(10);
    digitalWrite(EXAMPLE_PIN_NUM_LCD_RST, LOW);
    delay(50);
    digitalWrite(EXAMPLE_PIN_NUM_LCD_RST, HIGH);
    delay(120);
}

// ============================================================
// LCD_Init — ST7789T3 inicializacijsko zaporedje za 240x320
// ============================================================
void LCD_Init() {
    pinMode(EXAMPLE_PIN_NUM_LCD_CS,  OUTPUT);
    pinMode(EXAMPLE_PIN_NUM_LCD_DC,  OUTPUT);
    pinMode(EXAMPLE_PIN_NUM_LCD_RST, OUTPUT);
    digitalWrite(EXAMPLE_PIN_NUM_LCD_CS, HIGH);

    SPI_Init();
    LCD_Reset();

    LCD_WriteCommand(0x11);   // Sleep Out
    delay(120);

    LCD_WriteCommand(0x36);   // Memory Data Access Control
    LCD_WriteData(0x00);      // Normal orientation (portrait)

    LCD_WriteCommand(0x3A);   // Interface Pixel Format
    LCD_WriteData(0x05);      // 16-bit RGB565

    LCD_WriteCommand(0xB2);   // Porch Setting
    LCD_WriteData(0x0C);
    LCD_WriteData(0x0C);
    LCD_WriteData(0x00);
    LCD_WriteData(0x33);
    LCD_WriteData(0x33);

    LCD_WriteCommand(0xB7);   // Gate Control
    LCD_WriteData(0x35);

    LCD_WriteCommand(0xBB);   // VCOM Setting
    LCD_WriteData(0x19);

    LCD_WriteCommand(0xC0);   // LCM Control
    LCD_WriteData(0x2C);

    LCD_WriteCommand(0xC2);   // VDV and VRH Command Enable
    LCD_WriteData(0x01);
    LCD_WriteData(0xFF);

    LCD_WriteCommand(0xC3);   // VRH Set
    LCD_WriteData(0x12);

    LCD_WriteCommand(0xC4);   // VDV Set
    LCD_WriteData(0x20);

    LCD_WriteCommand(0xC6);   // Frame Rate Control in Normal Mode
    LCD_WriteData(0x0F);      // 60Hz

    LCD_WriteCommand(0xD0);   // Power Control 1
    LCD_WriteData(0xA4);
    LCD_WriteData(0xA1);

    LCD_WriteCommand(0xE0);   // Positive Voltage Gamma Control
    LCD_WriteData(0xD0);
    LCD_WriteData(0x04);
    LCD_WriteData(0x0D);
    LCD_WriteData(0x11);
    LCD_WriteData(0x13);
    LCD_WriteData(0x2B);
    LCD_WriteData(0x3F);
    LCD_WriteData(0x54);
    LCD_WriteData(0x4C);
    LCD_WriteData(0x18);
    LCD_WriteData(0x0D);
    LCD_WriteData(0x0B);
    LCD_WriteData(0x1F);
    LCD_WriteData(0x23);

    LCD_WriteCommand(0xE1);   // Negative Voltage Gamma Control
    LCD_WriteData(0xD0);
    LCD_WriteData(0x04);
    LCD_WriteData(0x0C);
    LCD_WriteData(0x11);
    LCD_WriteData(0x13);
    LCD_WriteData(0x2C);
    LCD_WriteData(0x3F);
    LCD_WriteData(0x44);
    LCD_WriteData(0x51);
    LCD_WriteData(0x2F);
    LCD_WriteData(0x1F);
    LCD_WriteData(0x1F);
    LCD_WriteData(0x20);
    LCD_WriteData(0x23);

    LCD_WriteCommand(0x21);   // Display Inversion On
    LCD_WriteCommand(0x29);   // Display On

    // Pobriši ekran na črno
    LCD_SetCursor(0, 0, LCD_WIDTH-1, LCD_HEIGHT-1);
    uint16_t black = 0x0000;
    for (uint32_t i = 0; i < (uint32_t)LCD_WIDTH * LCD_HEIGHT; i++) {
        LCD_WriteData(black >> 8);
        LCD_WriteData(black & 0xFF);
    }

    Serial.println("LCD_Init OK (240x320 ST7789T3)");
}

// ============================================================
// LCD_SetCursor — nastavi območje pisanja
// ============================================================
void LCD_SetCursor(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2) {
    LCD_WriteCommand(0x2A);   // Column Address Set
    LCD_WriteData(x1 >> 8);
    LCD_WriteData(x1 & 0xFF);
    LCD_WriteData(x2 >> 8);
    LCD_WriteData(x2 & 0xFF);

    LCD_WriteCommand(0x2B);   // Row Address Set
    LCD_WriteData(y1 >> 8);
    LCD_WriteData(y1 & 0xFF);
    LCD_WriteData(y2 >> 8);
    LCD_WriteData(y2 & 0xFF);

    LCD_WriteCommand(0x2C);   // Memory Write
}

// ============================================================
// LCD_addWindow — LVGL flush callback kliče to funkcijo
// ============================================================
void LCD_addWindow(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t* color) {
    uint32_t len = (uint32_t)(x2 - x1 + 1) * (y2 - y1 + 1) * sizeof(uint16_t);
    LCD_SetCursor(x1, y1, x2, y2);
    LCD_WriteData_nbyte((uint8_t*)color, NULL, len);
}

// ============================================================
// Backlight — PWM prek LEDC
// ============================================================
void Backlight_Init() {
    ledcSetup(LEDC_CHANNEL, LEDC_FREQ, LEDC_RES);
    ledcAttachPin(LCD_Backlight_PIN, LEDC_CHANNEL);
    ledcWrite(LEDC_CHANNEL, 1023);  // Polna svetlost ob inicializaciji
}

void Set_Backlight(uint8_t light) {
    if (light > Backlight_MAX) light = Backlight_MAX;
    uint32_t duty = (uint32_t)light * 1023 / 100;
    ledcWrite(LEDC_CHANNEL, duty);
    LCD_Backlight = light;
}