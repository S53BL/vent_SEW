// globals.cpp - Definicije globalnih spremenljivk in NVS upravljanje za vent_SEW
//
// POZOR: logBuffer in loggingInitialized sta definirani v logging.cpp.
//        globals.cpp jih NE definira - le extern deklaracija je v globals.h.
//        myTZ je definiran tukaj, inicializiran (setLocation) pa v main.cpp.
//
// POPRAVKI (2026-02-28):
//   - Dodane definicije timing spremenljivk: lastSensorReadMs, lastSendMs,
//     lastWifiCheckMs, lastNtpSyncMs, lastMotionMs, lastTouchMs,
//     lastConnectionFailMs, retryAttempted
//
// POPRAVKI (2026-03-01):
//   - lastMotionMs spremenjen v volatile unsigned long
//     Razlog: record_task v cam.cpp (FreeRTOS task, Core 1) bere lastMotionMs
//     medtem ko loop() kontekst (sens.cpp readSensors) vanj piše ob PIR zaznavi.
//     Brez volatile bi compiler cachiral vrednost v registru record_taska in
//     ne bi videl sprememb iz loop() → kamera bi napačno ustavila snemanje!
//   - checkAllDevices() dopolnjena s Camera statusom

#include "globals.h"
#include "logging.h"
#include <WiFi.h>

// --- Senzorski podatki in nastavitve ---
SensorData sensorData;
Settings   settings;

// --- ezTime: timezone objekt, lastnik je globals.cpp ---
// main.cpp pokliče myTZ.setLocation("Europe/Ljubljana") po NTP sinhronizaciji
Timezone myTZ;

// --- I2C reference ---
TwoWire& I2C_Internal = Wire;
TwoWire& I2C_Sensors  = Wire1;

// --- Cas ---
bool timeSynced = false;

// --- Stanje sistema ---
bool webServerRunning   = false;
bool screenOn           = true;
bool connection_ok      = false;

// --- Prisotnost senzorjev ---
bool sht41Present  = false;
bool bme680Present = false;
bool tcsPresent    = false;
bool sdPresent     = false;

// --- newSensorData flag ---
bool newSensorData = false;

// --- Timing ---
// FIX (2026-03-01): lastMotionMs MORA biti volatile!
//   Piše: loop() kontekst - sens.cpp readSensors() ob PIR zaznavi
//   Bere: FreeRTOS record_task (cam.cpp) v performMotionRecordingCheck()
//   Brez volatile: compiler optimizer cachira vrednost v register record_taska,
//   spremembe iz loop() niso vidne → kamera ne ve za nova gibanja!
volatile unsigned long lastMotionMs     = 0;

// PIR: čas zadnjega zaključenega gibanja (FALLING EDGE timestamp)
// 0 = ni še nobene zaznave od zagona
volatile time_t completedMotionTime     = 0;

// Nova timing spremenljivka: hitra zanka (1s — BSEC2 + PIR + BAT)
unsigned long lastFastTickMs       = 0;

// Ostale timing spremenljivke (samo loop() kontekst - volatile ni potreben)
unsigned long lastSensorReadMs     = 0;  // DEPRECATED
unsigned long lastSendMs           = 0;
unsigned long lastWifiCheckMs      = 0;
unsigned long lastNtpSyncMs        = 0;
unsigned long lastTouchMs          = 0;
unsigned long lastConnectionFailMs = 0;
bool retryAttempted                = false;

// Graf: glavna 3-minutna zanka
unsigned long lastMainCycleMs  = 0;
int           currentGraphHours = 4;   // privzeto: 4h okno

// --- SD mutex (logging.cpp in sd_card.cpp ga rabita) ---
SemaphoreHandle_t sdMutex = NULL;

// --- WiFi SSID (main.cpp ga nastavi ob uspešni povezavi) ---
String wifiSSID = "";

// POZOR: logBuffer, loggingInitialized, currentLogFile so definirani
// v logging.cpp (lastnik modula). Tukaj jih NE definiramo.

// =============================================================================
// CRC16 za NVS zaščito
// =============================================================================
static uint16_t calculateCRC(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
    }
    return crc;
}

// =============================================================================
// PRIVZETE VREDNOSTI
// =============================================================================
static void initDefaults() {
    strncpy(settings.unitId,  SETTINGS_DEFAULT_UNIT_ID,  sizeof(settings.unitId)  - 1);
    strncpy(settings.localIP, SETTINGS_DEFAULT_LOCAL_IP, sizeof(settings.localIP) - 1);
    strncpy(settings.gateway, SETTINGS_DEFAULT_GATEWAY,  sizeof(settings.gateway) - 1);
    strncpy(settings.rewIP,   SETTINGS_DEFAULT_REW_IP,   sizeof(settings.rewIP)   - 1);
    settings.unitId[sizeof(settings.unitId)   - 1] = '\0';
    settings.localIP[sizeof(settings.localIP) - 1] = '\0';
    settings.gateway[sizeof(settings.gateway) - 1] = '\0';
    settings.rewIP[sizeof(settings.rewIP)     - 1] = '\0';

    settings.tempOffset = settings.humOffset = settings.pressOffset = settings.luxOffset = 0.0f;
    settings.screenAlwaysOn   = false;
    settings.screenBrightness = SETTINGS_DEFAULT_BRIGHTNESS;
    settings.sendIntervalSec  = SETTINGS_DEFAULT_SEND_INTERVAL;
    settings.readIntervalSec  = SETTINGS_DEFAULT_READ_INTERVAL;
    settings.videoKeepDays    = 7;
    settings.reserved1 = settings.reserved2 = 0.0f;
    settings.reservedBool1 = false;

    LOG_INFO("Settings", "Defaults: id=%s ip=%s gw=%s rew=%s",
             settings.unitId, settings.localIP, settings.gateway, settings.rewIP);
}

// =============================================================================
// LOAD / SAVE / RESET SETTINGS
// =============================================================================
void loadSettings() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    uint8_t marker = prefs.getUChar("marker", 0);
    if (marker != 0xAB) {
        LOG_WARN("Settings", "NVS marker invalid - defaults");
        prefs.end(); initDefaults(); saveSettings(); return;
    }
    size_t bytesRead = prefs.getBytes("cfg", &settings, sizeof(Settings));
    uint16_t storedCRC = prefs.getUShort("crc", 0);
    prefs.end();

    if (bytesRead != sizeof(Settings)) {
        LOG_WARN("Settings", "NVS size mismatch (%d != %d) - defaults",
                 bytesRead, sizeof(Settings));
        initDefaults(); saveSettings(); return;
    }
    uint16_t calcCRC = calculateCRC((uint8_t*)&settings, sizeof(Settings));
    if (calcCRC != storedCRC) {
        LOG_WARN("Settings", "NVS CRC mismatch - defaults");
        initDefaults(); saveSettings(); return;
    }
    LOG_INFO("Settings", "Loaded: id=%s ip=%s gw=%s rew=%s (CRC=0x%04X)",
             settings.unitId, settings.localIP, settings.gateway, settings.rewIP, calcCRC);
}

void saveSettings() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putUChar("marker", 0xAB);
    prefs.putBytes("cfg", &settings, sizeof(Settings));
    uint16_t crc = calculateCRC((uint8_t*)&settings, sizeof(Settings));
    prefs.putUShort("crc", crc);
    prefs.end();
    LOG_INFO("Settings", "Saved: id=%s ip=%s (CRC=0x%04X)",
             settings.unitId, settings.localIP, crc);
}

void resetSettings() {
    LOG_WARN("Settings", "Reset to defaults!");
    initDefaults(); saveSettings();
}

// =============================================================================
// INIT GLOBALS
// =============================================================================
void initGlobals() {
    // Najprej naloži settings iz NVS
    loadSettings();

    // Počisti sensorData na varno začetno stanje
    memset(&sensorData, 0, sizeof(SensorData));
    strncpy(sensorData.unitId, settings.unitId, sizeof(sensorData.unitId) - 1);
    sensorData.unitId[sizeof(sensorData.unitId) - 1] = '\0';

    sensorData.temp        = ERR_FLOAT;
    sensorData.hum         = ERR_FLOAT;
    sensorData.press       = ERR_FLOAT;
    sensorData.iaq         = 0;
    sensorData.iaqAccuracy = 0;
    sensorData.staticIaq   = 0;
    sensorData.eCO2        = ERR_FLOAT;
    sensorData.breathVOC   = ERR_FLOAT;
    sensorData.lux         = ERR_FLOAT;
    sensorData.cct = sensorData.r = sensorData.g = sensorData.b = 0;
    sensorData.motion      = false;
    sensorData.motionCount = 0;
    sensorData.bat         = ERR_FLOAT;
    sensorData.batPct      = 0;
    sensorData.err         = ERR_NONE;
    sensorData.lastHttpCode = 0;
    sensorData.lastSendMs   = 0;
    sensorData.cameraReady  = false;
    sensorData.isRecording  = false;
    sensorData.recordingFrames = 0;
    sensorData.lastRecordingFile[0] = '\0';
    sensorData.reserved1 = sensorData.reserved2 = ERR_FLOAT;

    newSensorData = false;

    // Reset vseh timing spremenljivk
    lastFastTickMs       = 0;
    lastSensorReadMs     = 0;   // DEPRECATED
    lastSendMs           = 0;
    lastWifiCheckMs      = 0;
    lastNtpSyncMs        = 0;
    lastMotionMs         = 0;
    lastTouchMs          = 0;
    lastConnectionFailMs = 0;
    retryAttempted       = false;
    lastMainCycleMs      = 0;
    currentGraphHours    = 4;

    sdMutex = xSemaphoreCreateMutex();
    if (!sdMutex) LOG_ERROR("Globals", "SD mutex failed!");

    LOG_INFO("Globals", "Init OK - unit=%s ip=%s gw=%s rew=%s",
             settings.unitId, settings.localIP, settings.gateway, settings.rewIP);
}

// =============================================================================
// STARTUP VALIDACIJA
// =============================================================================
void checkAllDevices() {
    LOG_INFO("System", "=== STARTUP VALIDATION ===");
    int issues = 0;

    if (WiFi.status() != WL_CONNECTED) {
        LOG_ERROR("System", "WiFi not connected"); issues++;
    } else {
        LOG_INFO("System", "WiFi: %s  IP: %s", wifiSSID.c_str(),
                 WiFi.localIP().toString().c_str());
    }

    if (!timeSynced) LOG_WARN("System", "NTP not yet synced");
    else             LOG_INFO("System", "NTP synced");

    if (!sht41Present)  { LOG_ERROR("System", "SHT41 not detected"); issues++; }
    else                  LOG_INFO("System", "SHT41 OK");

    if (!bme680Present) LOG_WARN("System", "BME680 not detected");
    else                LOG_INFO("System", "BME680+BSEC OK (accuracy=%d)", sensorData.iaqAccuracy);

    if (!tcsPresent)    LOG_WARN("System", "TCS34725 not detected");
    else                LOG_INFO("System", "TCS34725 OK");

    if (!sdPresent)     LOG_WARN("System", "SD not accessible");
    else                LOG_INFO("System", "SD OK");

    if (!sensorData.cameraReady) LOG_WARN("System", "Camera not ready");
    else                          LOG_INFO("System", "Camera OK (stream: :81/stream)");

    if (issues == 0)
        LOG_INFO("System", "ALL OK  unit=%s", settings.unitId);
    else
        LOG_WARN("System", "READY WITH %d ISSUE(S)  unit=%s", issues, settings.unitId);

    LOG_INFO("System", "=== VALIDATION COMPLETE ===");
}
