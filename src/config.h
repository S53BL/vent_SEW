// config.h - Centralna konfiguracija za vent_SEW
// Plošča: Waveshare ESP32-S3-Touch-LCD-2
// Shema: https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2/ESP32-S3-Touch-LCD-2-SchDoc.pdf
//
// POZOR: Ta datoteka vsebuje SAMO hardware pinout, konstante in enum-e.
// Podatkovne strukture (SensorData, Settings) so definirane v globals.h.

#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// =============================================================================
// PINOUT - Waveshare ESP32-S3-Touch-LCD-2
// =============================================================================
//
//  ┌─────────────────────────────────────────────────────────────────────────┐
//  │  ZASLON (LCD) - ST7789T3, SPI vmesnik                                   │
//  │  Skupni SPI bus z SD kartico (MOSI=IO38, SCLK=IO39)                    │
//  ├─────────────┬────────────────────────────────────────────────────────── │
//  │  LCD_MOSI   │ IO38  - SPI MOSI (skupni z SD)                            │
//  │  LCD_SCLK   │ IO39  - SPI SCLK (skupni z SD)                            │
//  │  LCD_CS     │ IO45  - Chip Select zaslona                                │
//  │  LCD_DC     │ IO42  - Data/Command                                       │
//  │  LCD_RST    │ IO0   - Reset zaslona (vezano na BOOT tipko!)              │
//  │  LCD_BL     │ IO1   - Podsvietlitev (PWM)                                │
//  └─────────────┴──────────────────────────────────────────────────────────┘
//
//  ┌─────────────────────────────────────────────────────────────────────────┐
//  │  TOUCH - CST816D, I2C vmesnik (ločen I2C bus od senzorjev!)            │
//  ├─────────────┬───────────────────────────────────────────────────────────┤
//  │  TP_SDA     │ IO48  - Touch I2C SDA                                     │
//  │  TP_SCL     │ IO47  - Touch I2C SCL                                     │
//  │  TP_INT     │ IO46  - Touch interrupt                                   │
//  │  TP_RST     │ (vezan na LCD_RST = IO0)                                  │
//  └─────────────┴──────────────────────────────────────────────────────────┘
//
//  ┌─────────────────────────────────────────────────────────────────────────┐
//  │  SD KARTICA - SPI vmesnik (skupni SPI bus z LCD)                        │
//  ├─────────────┬───────────────────────────────────────────────────────────┤
//  │  SD_MOSI    │ IO38  - SPI MOSI (skupni z LCD)                           │
//  │  SD_SCLK    │ IO39  - SPI SCLK (skupni z LCD)                           │
//  │  SD_MISO    │ IO40  - SPI MISO (samo SD)                                │
//  │  SD_CS      │ IO41  - Chip Select SD kartice                            │
//  └─────────────┴──────────────────────────────────────────────────────────┘
//
//  ┌─────────────────────────────────────────────────────────────────────────┐
//  │  KAMERA - DVP vmesnik (OV2640 / OV5640, 24-pin FPC)                    │
//  ├─────────────┬───────────────────────────────────────────────────────────┤
//  │  CAM_D0     │ IO12  - Data bit 0                                        │
//  │  CAM_D1     │ IO13  - Data bit 1                                        │
//  │  CAM_D2     │ IO15  - Data bit 2                                        │
//  │  CAM_D3     │ IO11  - Data bit 3                                        │
//  │  CAM_D4     │ IO14  - Data bit 4                                        │
//  │  CAM_D5     │ IO10  - Data bit 5                                        │
//  │  CAM_D6     │ IO7   - Data bit 6                                        │
//  │  CAM_D7     │ IO2   - Data bit 7                                        │
//  │  CAM_PCLK   │ IO9   - Pixel clock                                       │
//  │  CAM_VSYNC  │ IO6   - Vertical sync                                     │
//  │  CAM_HREF   │ IO4   - Horizontal reference                              │
//  │  CAM_XCLK   │ IO8   - Master clock (output)                             │
//  │  TWI_SDA    │ IO21  - Camera I2C SDA (SCCB)                             │
//  │  TWI_CLK    │ IO16  - Camera I2C SCL (SCCB)                             │
//  │  CAM_PWDN   │ IO17  - Power down (active HIGH)                          │
//  └─────────────┴──────────────────────────────────────────────────────────┘
//
//  ┌─────────────────────────────────────────────────────────────────────────┐
//  │  IMU - QMI8658C, I2C vmesnik (skupen I2C bus s Touch!)                 │
//  ├─────────────┬───────────────────────────────────────────────────────────┤
//  │  IMU_SDA    │ IO48  - I2C SDA  (= TP_SDA - isti bus!)                  │
//  │  IMU_SCL    │ IO47  - I2C SCL  (= TP_SCL - isti bus!)                  │
//  │  IMU_INT1   │ IO3   - Interrupt 1                                       │
//  │             │  I2C naslov: 0x6A (SA0=GND) ali 0x6B (SA0=3V3)           │
//  └─────────────┴──────────────────────────────────────────────────────────┘
//
//  ┌─────────────────────────────────────────────────────────────────────────┐
//  │  BATERIJA                                                               │
//  ├─────────────┬───────────────────────────────────────────────────────────┤
//  │  BAT_ADC    │ IO5   - Napetost baterije (delilnik 200K/100K → x3)       │
//  └─────────────┴──────────────────────────────────────────────────────────┘
//
//  ┌─────────────────────────────────────────────────────────────────────────┐
//  │  SPLOŠNI GPIO (prosti pini za senzorje in V/I)                          │
//  ├─────────────┬───────────────────────────────────────────────────────────┤
//  │  IO33       │ I2C SDA zunanji senzorji (Wire1)                         │
//  │  IO34       │ I2C SCL zunanji senzorji (Wire1)                         │
//  │  IO35       │ PIR senzor gibanja (digitalni vhod)                      │
//  │  IO36       │ Status LED (digitalni izhod)                             │
//  │  IO37       │ Rezerva digitalni V/I                                    │
//  │  IO43       │ U0TXD - UART0 TX (Serial, USB-CDC)                        │
//  │  IO44       │ U0RXD - UART0 RX (Serial, USB-CDC)                        │
//  └─────────────┴──────────────────────────────────────────────────────────┘
//
//  ┌─────────────────────────────────────────────────────────────────────────┐
//  │  I2C RAZPOREDITEV - POZOR: 2 ločena busa!                              │
//  ├─────────────┬───────────────────────────────────────────────────────────┤
//  │  I2C_BUS_0  │ SDA=IO48, SCL=IO47 → Touch (CST816D) + IMU (QMI8658)     │
//  │  I2C_BUS_1  │ SDA=IO33, SCL=IO34 → Zunanji senzorji (SHT41, BME680...) │
//  └─────────────┴──────────────────────────────────────────────────────────┘
//
//  OPOMBA: IO18, IO19, IO20 = USB D-, D+ (ne uporabljati!)
//  OPOMBA: IO0 = BOOT tipka / LCD_RST (paziti pri zagonu)
// =============================================================================

// -----------------------------------------------------------------------------
// ZASLON - ST7789T3, SPI
// -----------------------------------------------------------------------------
#define LCD_MOSI_PIN        38
#define LCD_SCLK_PIN        39
#define LCD_CS_PIN          45
#define LCD_DC_PIN          42
#define LCD_RST_PIN          0   // POZOR: skupen z BOOT tipko!
#define LCD_BL_PIN           1   // Podsvietlitev, PWM

#define LCD_WIDTH           240
#define LCD_HEIGHT          320
#define LCD_ROTATION          0  // 0=portrait
#define LCD_PIXEL_CLOCK_HZ  (20 * 1000 * 1000)
#define LCD_CMD_BITS          8
#define LCD_PARAM_BITS        8

// LEDC za podsvietlitev LCD
#define LEDC_BL_CHANNEL       0  // Kanal 0 = LCD podsvietlitev
#define LEDC_BL_FREQ       5000
#define LEDC_BL_RES          10  // 10-bit = 0..1023

// FIX (2026-02-28): LEDC za kamero - eksplicitni defines preprečijo magic numbers v cam.cpp
// POZOR: kanal mora biti RAZLIČEN od LEDC_BL_CHANNEL (0)!
// Kanal 2 = kamera XCLK (LEDC_CHANNEL_2)
// Timer  2 = kamera timer (LEDC_TIMER_2)
#define CAM_LEDC_CHANNEL      2  // LEDC_CHANNEL_2 - ne sme biti 0 (=LCD BL)!
#define CAM_LEDC_TIMER        2  // LEDC_TIMER_2   - ne sme biti 0 (=LCD BL timer)

// LVGL
#define LVGL_DRAW_BUF_HEIGHT  60
#define LVGL_DRAW_BUF_DOUBLE   1

// -----------------------------------------------------------------------------
// TOUCH - CST816D, I2C (I2C bus 0)
// -----------------------------------------------------------------------------
#define TP_SDA_PIN          48
#define TP_SCL_PIN          47
#define TP_INT_PIN          46
#define CST816D_ADDR      0x15

// -----------------------------------------------------------------------------
// SD KARTICA - SPI (skupni bus z LCD)
// -----------------------------------------------------------------------------
#define SD_MOSI_PIN         38
#define SD_SCLK_PIN         39
#define SD_MISO_PIN         40
#define SD_CS_PIN           41

// -----------------------------------------------------------------------------
// KAMERA - DVP (OV2640 / OV5640)
// -----------------------------------------------------------------------------
#define CAM_D0_PIN          12
#define CAM_D1_PIN          13
#define CAM_D2_PIN          15
#define CAM_D3_PIN          11
#define CAM_D4_PIN          14
#define CAM_D5_PIN          10
#define CAM_D6_PIN           7
#define CAM_D7_PIN           2
#define CAM_PCLK_PIN         9
#define CAM_VSYNC_PIN        6
#define CAM_HREF_PIN         4
#define CAM_XCLK_PIN         8
#define CAM_SDA_PIN         21   // TWI_SDA (SCCB)
#define CAM_SCL_PIN         16   // TWI_CLK (SCCB)
#define CAM_PWDN_PIN        17   // Power down, active HIGH



// -----------------------------------------------------------------------------
// IMU - QMI8658C, I2C (skupen bus s Touch - I2C bus 0)
// -----------------------------------------------------------------------------
#define IMU_INT1_PIN         3
#define QMI8658_ADDR      0x6A   // SA0=GND; ce SA0=3V3 → 0x6B

// -----------------------------------------------------------------------------
// I2C BUSA
// -----------------------------------------------------------------------------
// Bus 0: Touch (CST816D) + IMU (QMI8658)
#define I2C_TOUCH_IMU_SDA   TP_SDA_PIN   // IO48
#define I2C_TOUCH_IMU_SCL   TP_SCL_PIN   // IO47

// Bus 1: Zunanji senzorji (SHT41, BME680, TCS34725)
#define I2C_SENS_SDA        33
#define I2C_SENS_SCL        34

#define I2C_CLOCK_SPEED   10000  // 10 kHz - robustno za daljse kable/sum
#define I2C_TIMEOUT_MS      100

// -----------------------------------------------------------------------------
// ZUNANJA APLIKACIJSKA GPIO (SEW specificno)
// -----------------------------------------------------------------------------
#define PIR_PIN             35   // PIR senzor gibanja, digitalni vhod
#define STATUS_LED_PIN      36   // Status LED, digitalni izhod
#define GPIO_EXT1_PIN       37   // Rezerva

// Kompatibilnostni alias (stara koda)
#define MOTION_SENSOR_PIN   PIR_PIN

// -----------------------------------------------------------------------------
// BATERIJA
// -----------------------------------------------------------------------------
#define BAT_ADC_PIN          5
#define BAT_VOLTAGE_DIVIDER  3.0f   // Delilnik 200K/100K

// -----------------------------------------------------------------------------
// SENZORJI - I2C naslovi (na I2C bus 1)
// -----------------------------------------------------------------------------
#define SHT41_ADDRESS       0x44
#define BME680_ADDRESS      0x76   // SDO=GND (BSEC privzeto)
#define BME680_ADDR_HIGH    0x77   // SDO=VCC
#define TCS_ADDRESS         0x29   // TCS34725

#define SENSOR_RETRY_COUNT    3
#define SENSOR_RETRY_DELAY  200    // ms med poizkusi

// -----------------------------------------------------------------------------
// WIFI & OMREZJE
// -----------------------------------------------------------------------------
extern const char*  ssidList[];
extern const char*  passwordList[];
extern const int    numNetworks;

#define SEW_IP              "192.168.2.191"
#define REW_IP              "192.168.2.190"
#define CEE_IP              "192.168.2.192"
#define REW_DATA_ENDPOINT   "/data"

// -----------------------------------------------------------------------------
// CASOVNI INTERVALI (ms)
// -----------------------------------------------------------------------------
#define SENSOR_READ_INTERVAL    30000UL   // Branje senzorjev: 30 s
#define DATA_SEND_INTERVAL     180000UL   // Posiljanje na REW: 3 min
#define WIFI_CHECK_INTERVAL    600000UL   // Preverjanje WiFi: 10 min
#define SCREEN_TIMEOUT          60000UL   // Izklop zaslona po 60 s
#define MOTION_DEBOUNCE           500UL   // PIR debounce: 500 ms
#define NTP_UPDATE_INTERVAL   1800000UL   // NTP sinhronizacija: 30 min

// Casovna cona
#define TZ_STRING   "CET-1CEST,M3.5.0,M10.5.0/3"

// -----------------------------------------------------------------------------
// UART
// -----------------------------------------------------------------------------
#define UART_TX_PIN         43
#define UART_RX_PIN         44

// -----------------------------------------------------------------------------
// NAPAKE (bitmask)
// -----------------------------------------------------------------------------
enum ErrorFlag : uint8_t {
    ERR_NONE    = 0x00,
    ERR_SHT41   = 0x01,
    ERR_BME680  = 0x02,
    ERR_TCS     = 0x04,
    ERR_WIFI    = 0x08,
    ERR_HTTP    = 0x10,
    ERR_SD      = 0x20,
    ERR_NTP     = 0x40,
    ERR_CAMERA  = 0x80,
};

// -----------------------------------------------------------------------------
// SENTINEL VREDNOSTI (neveljavne meritve)
// -----------------------------------------------------------------------------
#define ERR_FLOAT   -999.0f
#define ERR_INT     -999

// -----------------------------------------------------------------------------
// MEJE VELJAVNIH MERITEV
// -----------------------------------------------------------------------------
#define TEMP_MIN        -40.0f
#define TEMP_MAX         85.0f
#define HUM_MIN           0.0f
#define HUM_MAX         100.0f
#define PRESS_MIN       300.0f
#define PRESS_MAX      1200.0f
#define LUX_MIN           0.0f
#define LUX_MAX       65535.0f

#endif // CONFIG_H
