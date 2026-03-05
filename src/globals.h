// globals.h - Globalne spremenljivke in deklaracije za vent_SEW
//
// =============================================================================
// ARHITEKTURNI DOGOVORI (2025-02)
// =============================================================================
//
// IDENTITETA ENOTE:
//   - Vsak SEW ima unikatni ID (npr. "SEW1"..."SEW5") shranjen v NVS
//   - IP in gateway sta DETERMINISTIČNA — izračunata iz unitId v initGlobals() (sewIdToIP())
//   - Samo unitId se shranjuje v NVS; localIP in gateway se NIKOLI ne shranjujeta
//   - Firmware je identičen za vse SEW enote — razlikujejo se samo po unitId v NVS
//   - Default ob first-boot: SEW5 (192.168.2.199)
//   - Tabela: SEW1=.195, SEW2=.196, SEW3=.197, SEW4=.198, SEW5=.199 (config.h)
//
// KOMUNIKACIJSKI PROTOKOL SEW REW:
//   - SEW poslja HTTP POST na en endpoint: http://<REW_IP>/data
//   - En JSON paket vsebuje: identiteto enote + cel set senzorskih podatkov
//   - REW loci enote po "id" polju v JSON
//   - Interval posiljanja: settings.sendIntervalSec (privzeto 180 s)
//   - Format: { "id":"SEW1", "temp":12.3, "hum":65.1, "press":1013.2,
//               "iaq":85, "iaq_acc":3, "siaq":82, "eco2":650, "bvoc":0.5,
//               "lux":234, "cct":5500, "motion":1, "bat":3.85, "err":6, ... }
//
// BSEC (BME680):
//   - Sample rate: BSEC_SAMPLE_RATE_LP (~3s interval)
//   - State persistence: NVS namespace "sew_bsec"
//   - Shranjevanje: ob prvi accuracy>=3, nato vsakih STATE_SAVE_PERIOD_MS
//
// newSensorData FLAG:
//   - sens.cpp ga postavi na true po uspesnem branju senzorjev
//   - main.cpp ga prebere in postavi nazaj na false
//   - Uporablja se za graphAddPoint() klic SAMO ob dejansko novem branju
//
// I2C BUSA:
//   - Wire  (bus 0, IO48/IO47): Touch CST816D + IMU QMI8658 + Zunanji senzorji (SHT41, BME680, TCS34725)
//   - Wire1 (bus 1): NI VEČ V UPORABI (IO33/IO34 nista dostopni na konektorjih P1/P2)
//
// STORAGE:
//   - NVS "sew_cfg"  : unitId, rewIP, kalibracija, display settings, intervali
//                      (localIP in gateway se NE shranjujeta — sta computed iz unitId)
//   - NVS "sew_bsec" : BSEC state vektor
//   - SD kartica     : dnevni logi + CSV meritve
//
// FLASH LAYOUT (16MB):
//   nvs(32K) | otadata(8K) | ota_0(6.5M) | ota_1(6.5M) | littlefs(2.9M) | coredump(64K)
//
// LOGIRANJE (logBuffer / loggingInitialized):
//   - Spremenljivki sta definirani v logging.cpp (lastnik modula)
//   - globals.cpp jih NE definira - samo extern deklaracija tukaj
//   - logging.cpp ne sme imeti "static" pred njima ker so extern v globals.h
//
// myTZ (ezTime):
//   - Definiran v globals.cpp, deklariran tukaj
//   - Uporablja se v logging.cpp, sd_card.cpp za datum/cas formatiranje
//
// lastMotionMs (threading):
//   - MORA biti volatile ker jo bere record_task (FreeRTOS task, Core 1)
//     in pise loop() kontekst (sens.cpp readSensors())
//   - cam.cpp bere lastMotionMs direktno (ne _lastMotionMs!) - FIX 2026-02-28
//
// =============================================================================

#ifndef GLOBALS_H
#define GLOBALS_H

#include "config.h"
#include <Wire.h>
#include <Preferences.h>
#include <IPAddress.h>
#include <ezTime.h>          // Timezone, now(), dateTime()

// =============================================================================
// SENZORSKI PODATKI - struktura za en JSON paket
// =============================================================================

struct SensorData {
    // --- Identiteta (iz NVS settings) ---
    char unitId[8];         // "SEW1".."SEW9"

    // --- SHT41: temperatura in vlaznost (primarni senzor) ---
    float temp;             // Temperatura [C], korigirana s tempOffset
    float hum;              // Relativna vlaznost [%], korigirana s humOffset

    // --- BME680 + BSEC: tlak in kakovost zraka ---
    float press;            // Atmosferski tlak [hPa], korigiran s pressOffset
    // iaqAccuracy: 0=unreliable, 1=low, 2=medium, 3=calibrated (~24h)
    // staticIaq: priporocen za SEW (naprava fiksno montirana)
    uint16_t iaq;           // IAQ index 0-500
    uint8_t  iaqAccuracy;   // BSEC kalibracijska tocnost 0-3
    uint16_t staticIaq;     // Static IAQ
    float    eCO2;          // CO2 ekvivalent [ppm]
    float    breathVOC;     // Breath VOC ekvivalent [ppm]

    // --- TCS34725: svetloba ---
    float lux;              // Osvetljenost [lx], korigirana z luxOffset
    uint16_t cct;           // Barvna temperatura [K]
    uint16_t r, g, b;       // Raw RGB vrednosti

    // --- PIR: gibanje ---
    bool motion;            // true = gibanje zaznano v zadnjem intervalu
    uint16_t motionCount;   // Stevilo zaznav v zadnjem DATA_SEND_INTERVAL

    // --- Baterija ---
    float bat;              // Napetost baterije [V] (3.0V=prazna, 4.2V=polna)
    uint8_t batPct;         // Odstotek polnosti [%]

    // --- Napake (bitmask - ErrorFlag enum iz config.h) ---
    uint8_t err;

    // --- Stanje posiljanja ---
    int lastHttpCode;
    unsigned long lastSendMs;

    // --- Kamera ---
    bool     cameraReady;           // true = esp_camera_init() uspesen
    bool     isRecording;           // true = AVI snemanje aktivno
    uint32_t recordingFrames;       // stevilo posnetih frameov
    char     lastRecordingFile[40]; // npr. "2026-02-22_10-30-00.avi"

    // --- Rezerva za prihodnje senzorje ---
    float reserved1;
    float reserved2;
};

// =============================================================================
// NASTAVITVE - shranjene v NVS (Preferences.h, namespace "sew_cfg")
// =============================================================================

struct Settings {
    // --- Identiteta enote ---
    char unitId[8];         // "SEW1".."SEW9"

    // --- Omrezje ---
    // POZOR: localIP in gateway se NE shranjujeta v NVS!
    // Sta izračunana v initGlobals() iz unitId (sewIdToIP) in SEW_GATEWAY.
    // Tukaj sta samo za branje s strani main.cpp, web_handlers.cpp in http.cpp.
    char localIP[16];       // Computed: sewIdToIP(unitId) — ni NVS!
    char gateway[16];       // Computed: SEW_GATEWAY — ni NVS!
    char rewIP[16];         // IP REW enote, privzeto REW_IP — JE v NVS

    // --- Kalibracija SHT41 ---
    float tempOffset;       // [C]  privzeto: 0.0
    float humOffset;        // [%]   privzeto: 0.0

    // --- Kalibracija BME680 ---
    float pressOffset;      // [hPa] privzeto: 0.0

    // --- Kalibracija TCS34725 ---
    float luxOffset;        // [lx]  privzeto: 0.0

    // --- Zaslon ---
    bool screenAlwaysOn;
    uint16_t screenBrightness; // 0-1023, privzeto: 512

    // --- Intervali (v sekundah) ---
    uint16_t sendIntervalSec;  // privzeto: 180
    uint16_t readIntervalSec;  // privzeto: 30

    // --- Video hranjenje ---
    uint8_t videoKeepDays;  // Privzeto: 7 (dni hranjenja posnetkov)

    // --- Rezerva ---
    float reserved1;
    float reserved2;
    bool  reservedBool1;
};

// --- BSEC state NVS ---
#define BSEC_NVS_NAMESPACE      "sew_bsec"
#define BSEC_NVS_KEY_STATE      "state"
#define BSEC_NVS_KEY_VALID      "valid"
#define STATE_SAVE_PERIOD_MS    (10UL * 60UL * 1000UL)  // 10 min

// --- NVS namespace za settings ---
#define NVS_NAMESPACE                   "sew_cfg"

// --- Privzete vrednosti ---
// POZOR: localIP in gateway NI VEČ default — sta computed iz unitId (sewIdToIP v config.h)
#define SETTINGS_DEFAULT_UNIT_ID        "SEW5"   // First-boot default: SEW5 = 192.168.2.199
#define SETTINGS_DEFAULT_REW_IP         REW_IP
#define SETTINGS_DEFAULT_BRIGHTNESS     512
#define SETTINGS_DEFAULT_SEND_INTERVAL  180
#define SETTINGS_DEFAULT_READ_INTERVAL  30

// =============================================================================
// GLOBALNE SPREMENLJIVKE - extern deklaracije
// =============================================================================

extern SensorData sensorData;
extern Settings   settings;

// --- ezTime ---
// myTZ je definiran v globals.cpp, inicializiran v main.cpp (myTZ.setLocation)
// Vsi moduli (logging.cpp, sd_card.cpp) ga folosijo za datum/cas
extern Timezone myTZ;

// --- I2C busa ---
extern TwoWire& I2C_Internal;   // = Wire,  IO48/IO47 (Touch + IMU)
extern TwoWire& I2C_Sensors;    // = Wire1, IO48/IO47 (zunanji senzorji - isti bus kot Touch+IMU)

// --- Cas ---
extern bool timeSynced;

// --- Stanje sistema ---
extern bool webServerRunning;
extern bool screenOn;
extern bool connection_ok;

// --- Prisotnost senzorjev in perifernih naprav ---
extern bool sht41Present;
extern bool bme680Present;
extern bool tcsPresent;
extern bool sdPresent;       // SD kartica prisotna

// --- Flag za nov senzorski odcitek ---
// sens.cpp ga postavi na true po uspesnem readSensors()
// main.cpp ga ob zaznavi postavi na false in klice graphAddPoint()
extern bool newSensorData;

// --- Timing spremenljivke ---
// POZOR: lastMotionMs mora biti volatile!
//   - Pise: loop() kontekst - sens.cpp readSensors() ob PIR zaznavi
//   - Bere: FreeRTOS record_task (Core 1) v cam.cpp performMotionRecordingCheck()
//   - Brez volatile bi optimizer lahko cachiral staro vrednost v record_task!
extern volatile unsigned long lastMotionMs;

// --- PIR: čas zadnjega zaključenega gibanja (FALLING EDGE timestamp) ---
// completedMotionTime: MORA biti volatile — piše readPIR() (loop kontekst),
//   bere disp.cpp updateUI() (isti kontekst, toda za konsistentnost)
// Vrednost 0 = ni še nobene zaznave od zagona
extern volatile time_t completedMotionTime;

// --- PIR: čas PREDHODNEGA zaključenega gibanja (za prikaz na zaslonu) ---
// previousMotionTime: shrani se ob vsakem RISING EDGE (nova zaznava) kot
//   kopija completedMotionTime PREDEN se ta posodobi z novim gibanjem.
// Namen: oseba, ki se priblíža (RISING EDGE), vidi čas PREDHODNE zaznave,
//   ne "NOW". Vrednost 0 = ni še predhodne zaznave.
extern volatile time_t previousMotionTime;

// Nova timing spremenljivka: hitra zanka (1s — BSEC2 + PIR + BAT)
// Ob zagonu se postavi na 0 da se prva iteracija sproži takoj.
extern unsigned long lastFastTickMs;

// Ostale timing spremenljivke (samo loop() kontekst - volatile ni potreben)
// lastSensorReadMs: DEPRECATED — 30s monolitno branje zamenjano z ločenima zankama
extern unsigned long lastSensorReadMs;   // DEPRECATED
extern unsigned long lastSendMs;
extern unsigned long lastWifiCheckMs;
extern unsigned long lastNtpSyncMs;
extern unsigned long lastTouchMs;
extern unsigned long lastConnectionFailMs;
extern bool retryAttempted;

// --- Graf: glavna 3-minutna zanka ---
extern unsigned long lastMainCycleMs;  // čas zadnjega 3-min cikla
extern int           currentGraphHours; // trenutno prikazano časovno okno (2/4/8/16/24)

// --- Logging (definirano v logging.cpp, ne v globals.cpp!) ---
// POZOR: logBuffer in loggingInitialized sta definirani v logging.cpp
// globals.cpp jih NE sme definirati - samo extern deklaracija tukaj
extern String logBuffer;
extern bool   loggingInitialized;

// --- Pending WiFi reconnect po spremembi identitete (flag, identičen DEW) ---
// Postavi ga: http.cpp POST /api/settings ob spremembi unitId
// Bere ga:    main.cpp loop() — po 500ms kliče WiFi.config + connectWifi()
extern bool pendingWiFiReconnect;

// --- SD mutex (definiran v globals.cpp) ---
extern SemaphoreHandle_t sdMutex;

// --- WiFi ---
extern String wifiSSID;

// --- Log datoteka (definirana v logging.cpp) ---
extern String currentLogFile;

// =============================================================================
// FUNKCIJE
// =============================================================================

void initGlobals();
void loadSettings();
void saveSettings();
void resetSettings();
void checkAllDevices();

#endif // GLOBALS_H
