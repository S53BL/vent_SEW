// sens.cpp - Sensor module implementation for vent_SEW
//
// Senzorji na I2C_Sensors (Wire, IO48/IO47 - skupen bus):
//   CST816D  (0x15) - touch (inicializira LVGL_Driver.cpp)
//   QMI8658  (0x6A) - IMU
//   SHT41    (0x44) - temperatura, vlažnost
//   BME680   (0x76) - tlak, IAQ, eCO2, breathVOC (via BSEC2)
//   TCS34725 (0x29) - lux, CCT, RGB
//   PIR      (IO18) - gibanje → ob detekciji kliče startMotionRecording()
//
// BSEC2 inicializacijski tok (OBVEZEN VRSTNI RED!):
//   1. iaqSensor.begin(address, Wire)
//   2. iaqSensor.setConfig(bsec_config_iaq)   ← NOVO v BSEC2, manjkalo v v1!
//   3. loadBsecState() → iaqSensor.setState()
//   4. iaqSensor.attachCallback(bsecDataCallback)
//   5. iaqSensor.updateSubscription(list, count, BSEC_SAMPLE_RATE_LP)
//
// BSEC2 podatki prihajajo skozi callback (ne polling member spremenljivk):
//   - bsecDataCallback() prejme outputs strukturo
//   - switch(o.sensor_id) razporeja vrednosti v sensorData
//
// newSensorData flag:
//   - Po vsakem uspešnem readSensors() se postavi na true
//   - main.cpp ga prebere za graphAddPoint() in postavi nazaj na false
//   - "Uspešno branje" = vsaj SHT41 je prebral brez napake
//
// BSEC state persistence (NVS "sew_bsec"):
//   - Ob zagonu: naloži state → BSEC nadaljuje kalibracijo brez reseta
//   - Shranjevanje: ob prvi iaqAccuracy>=3, nato vsakih STATE_SAVE_PERIOD_MS
//   - BSEC_SAMPLE_RATE_LP: meritev vsakih ~3s
//
// PIR → kamera integracija:
//   - Vsak RISING edge kliče startMotionRecording() iz cam.cpp
//   - startMotionRecording() je idempotenten
//   - Polling v readSensors() zadošča (PIR drži HIGH vsaj 2-3s)
//
// I2C recovery:
//   - checkI2CDevice() pred vsakim branjem
//   - resetI2CBus(): 9x SCL toggle + Wire.end/begin
//   - error counter: absent po SENSOR_RETRY_COUNT napakah
//   - performPeriodicSensorCheck(): reconnect vsakih 10 min
//   - performPeriodicI2CReset(): preventivni reset vsakih 30 min
//
// Baterija:
//   - ADC IO5, delilnik 200K/100K → faktor x3, povprečje 16 vzorcev
//
// MIGRACIJA (2026-03-03):
//   - BSEC Software Library v1.x → Bosch-BSEC2-Library
//   - Razlog: v1.x nima ESP32-S3 binarije (xtensa-esp32s3-elf-gcc)
//   - Spremembe: #include <bsec2.h>, Bsec2 razred, setConfig(), attachCallback()
//   - Vmesnik navzven (sens.h) se NI spremenil
//
// POPRAVKI (2026-03-02):
//   - FIX: TCS34725 konstruktor - samo 2 parametra, &Wire gre v begin()
//   - FIX: #include <SensirionI2cSht4x.h> (malo 'c', malo 'x')
//   - FIX: Wire (IO48/IO47) - skupen bus, inicializira Touch_Init()

#include "sens.h"
#include "cam.h"       // startMotionRecording()
#include "globals.h"
#include "logging.h"
#include <Wire.h>
#include <SD.h>        // logMotionEvent() — zapis na SD kartico
#include <SensirionI2cSht4x.h>
#include <bsec2.h>                    // BSEC2 (Bosch-BSEC2-Library)
#include <Adafruit_TCS34725.h>
#include <Preferences.h>

// BSEC2 config blob - za BME680, 3.3V, LP (3s), 4-day model
// Ta header je del Bosch-BSEC2-Library, PlatformIO ga najde avtomatično
const uint8_t bsec_config_iaq[] = {
#include "config/bme680/bme680_iaq_33v_3s_4d/bsec_iaq.txt"
};

// ============================================================
// Senzorski objekti
// ============================================================
static SensirionI2cSht4x* sht41 = nullptr;
static Bsec2              iaqSensor;    // BSEC2: Bsec2 razred
static Adafruit_TCS34725* tcs   = nullptr;

// Error counters
static uint8_t sht41ErrorCount  = 0;
static uint8_t bme680ErrorCount = 0;
static uint8_t tcsErrorCount    = 0;

// BSEC state tracking
static unsigned long lastStateSaveMs  = 0;
static uint8_t       stateUpdateCount = 0;  // 0 = čaka na prvo accuracy>=3

// PIR tracking (za edge detection)
static bool pirLastState = false;

// ============================================================
// BSEC2 subscription lista
// ============================================================
static bsec_virtual_sensor_t bsecSensorList[] = {
    BSEC_OUTPUT_RAW_PRESSURE,
    BSEC_OUTPUT_RAW_TEMPERATURE,
    BSEC_OUTPUT_RAW_HUMIDITY,
    BSEC_OUTPUT_COMPENSATED_GAS,
    BSEC_OUTPUT_IAQ,
    BSEC_OUTPUT_STATIC_IAQ,
    BSEC_OUTPUT_CO2_EQUIVALENT,
    BSEC_OUTPUT_BREATH_VOC_EQUIVALENT,
    BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE,
    BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY,
};

// ============================================================
// I2C bus - skupen Wire (IO48/IO47)
// POZOR: Wire.begin() je že klican v Touch_Init() (LVGL_Driver.cpp)
//        pred klicem initSens(). Tukaj samo preverimo/nastavimo timeout.
// ============================================================
void initI2CBus() {
    // Wire je že inicializiran v Touch_Init() z Wire.begin(48, 47, 100000)
    // Samo nastavi timeout za robustnost
    Wire.setTimeout(I2C_TIMEOUT_MS);
    LOG_INFO("I2C", "Wire (shared bus) SDA=IO48 SCL=IO47 - already initialized by Touch_Init");
}

bool checkI2CDevice(uint8_t address) {
    Wire.beginTransmission(address);
    return (Wire.endTransmission() == 0);
}

void resetI2CBus() {
    // Standard I2C recovery: 9x SCL toggle
    // POZOR: IO47=SCL, IO48=SDA (skupen bus - Touch bo tudi offline med resetom)
    pinMode(I2C_TOUCH_IMU_SCL, OUTPUT);  // IO47
    pinMode(I2C_TOUCH_IMU_SDA, INPUT);   // IO48
    for (int i = 0; i < 9; i++) {
        digitalWrite(I2C_TOUCH_IMU_SCL, LOW);  delayMicroseconds(10);
        digitalWrite(I2C_TOUCH_IMU_SCL, HIGH); delayMicroseconds(10);
    }
    if (digitalRead(I2C_TOUCH_IMU_SDA) == LOW)
        LOG_ERROR("I2C", "SDA stuck low after recovery!");
    Wire.end();
    delay(10);
    Wire.begin(I2C_TOUCH_IMU_SDA, I2C_TOUCH_IMU_SCL, 100000);
    Wire.setTimeout(I2C_TIMEOUT_MS);
    LOG_INFO("I2C", "Bus reset complete (Wire IO48/IO47)");
}

// ============================================================
// BSEC STATE - load/save iz NVS
// ============================================================
static void loadBsecState() {
    Preferences prefs;
    prefs.begin(BSEC_NVS_NAMESPACE, true);

    bool valid = prefs.getBool(BSEC_NVS_KEY_VALID, false);
    if (!valid) {
        prefs.end();
        LOG_INFO("BSEC", "No saved state - starting fresh calibration");
        return;
    }

    uint8_t stateBlob[BSEC_MAX_STATE_BLOB_SIZE];
    size_t len = prefs.getBytes(BSEC_NVS_KEY_STATE, stateBlob, BSEC_MAX_STATE_BLOB_SIZE);
    prefs.end();

    if (len != BSEC_MAX_STATE_BLOB_SIZE) {
        LOG_WARN("BSEC", "State size mismatch (%d != %d) - ignoring",
                 len, BSEC_MAX_STATE_BLOB_SIZE);
        return;
    }

    if (!iaqSensor.setState(stateBlob)) {
        LOG_WARN("BSEC", "setState failed (status=%d) - starting fresh", iaqSensor.status);
    } else {
        LOG_INFO("BSEC", "State loaded from NVS - calibration continues");
    }
}

static void saveBsecState() {
    uint8_t stateBlob[BSEC_MAX_STATE_BLOB_SIZE];
    if (!iaqSensor.getState(stateBlob)) {
        LOG_WARN("BSEC", "getState failed (status=%d) - not saving", iaqSensor.status);
        return;
    }

    Preferences prefs;
    prefs.begin(BSEC_NVS_NAMESPACE, false);
    prefs.putBytes(BSEC_NVS_KEY_STATE, stateBlob, BSEC_MAX_STATE_BLOB_SIZE);
    prefs.putBool(BSEC_NVS_KEY_VALID, true);
    prefs.end();

    LOG_INFO("BSEC", "State saved to NVS (accuracy=%d, count=%d)",
             sensorData.iaqAccuracy, stateUpdateCount);
}

static void updateBsecState() {
    bool doSave = false;

    if (stateUpdateCount == 0) {
        // iaqAccuracy se polni v bsecDataCallback()
        if (sensorData.iaqAccuracy >= 3) {
            doSave = true;
            stateUpdateCount++;
            LOG_INFO("BSEC", "First full calibration reached! Saving state.");
        }
    } else {
        if (millis() - lastStateSaveMs >= STATE_SAVE_PERIOD_MS) {
            doSave = true;
            stateUpdateCount++;
        }
    }

    if (doSave) {
        saveBsecState();
        lastStateSaveMs = millis();
    }
}

// ============================================================
// BSEC2 CALLBACK
// Pokliče se avtomatično ko so novi podatki pripravljeni.
// POZOR: mora biti static (ali global) ker jo registriramo z attachCallback()
// ============================================================
static void bsecDataCallback(const bme68xData data,
                              const bsecOutputs outputs,
                              const Bsec2 bsec) {
    if (!outputs.nOutputs) return;

    bme680ErrorCount = 0;
    sensorData.err &= ~ERR_BME680;

    for (uint8_t i = 0; i < outputs.nOutputs; i++) {
        const bsecData o = outputs.output[i];
        switch (o.sensor_id) {
            case BSEC_OUTPUT_IAQ:
                sensorData.iaq         = (uint16_t)o.signal;
                sensorData.iaqAccuracy = o.accuracy;
                break;
            case BSEC_OUTPUT_STATIC_IAQ:
                sensorData.staticIaq = (uint16_t)o.signal;
                break;
            case BSEC_OUTPUT_CO2_EQUIVALENT:
                sensorData.eCO2 = o.signal;
                break;
            case BSEC_OUTPUT_BREATH_VOC_EQUIVALENT:
                sensorData.breathVOC = o.signal;
                break;
            case BSEC_OUTPUT_RAW_PRESSURE: {
                float hPa = o.signal / 100.0f;   // FIX: BSEC vrača Pa, ne hPa!
                if (hPa >= PRESS_MIN && hPa <= PRESS_MAX)
                    sensorData.press = hPa + settings.pressOffset;
                else
                    LOG_WARN("BSEC", "Invalid pressure: %.1f Pa (%.2f hPa)", o.signal, hPa);
                break;
            }
            case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE:
                // Opcijsko: kompenzirana temp iz BME (za cross-check s SHT41)
                break;
            case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY:
                // Opcijsko
                break;
            default:
                break;
        }
    }

    LOG_INFO("BSEC", "P=%.1fhPa IAQ=%u(acc=%d) sIAQ=%u eCO2=%.0f bVOC=%.2f",
             sensorData.press,
             sensorData.iaq, sensorData.iaqAccuracy,
             sensorData.staticIaq,
             sensorData.eCO2, sensorData.breathVOC);

    updateBsecState();
}

// ============================================================
// INICIALIZACIJA SENZORJEV
// ============================================================
bool initSensors() {
    initI2CBus();

    sensorData.err |= (ERR_SHT41 | ERR_BME680 | ERR_TCS);
    sht41Present = bme680Present = tcsPresent = false;

    // --- SHT41 ---
    if (checkI2CDevice(SHT41_ADDRESS)) {
        sht41 = new SensirionI2cSht4x();
        sht41->begin(Wire, SHT41_ADDRESS);
        sht41->softReset();
        delay(10);

        float t, h;
        uint16_t err = sht41->measureHighPrecision(t, h);
        if (!err && t > TEMP_MIN && t < TEMP_MAX && h >= HUM_MIN && h <= HUM_MAX) {
            sht41Present = true;
            sht41ErrorCount = 0;
            sensorData.err &= ~ERR_SHT41;
            LOG_INFO("SHT41", "OK T=%.1f H=%.1f (raw)", t, h);
        } else {
            LOG_WARN("SHT41", "Init read failed (err=%u)", err);
            delete sht41; sht41 = nullptr;
        }
    } else {
        LOG_WARN("SHT41", "Not on I2C (0x%02X)", SHT41_ADDRESS);
    }

    // --- BME680 + BSEC2 ---
    // OBVEZEN VRSTNI RED: begin → setConfig → loadBsecState → attachCallback → updateSubscription
    if (checkI2CDevice(BME680_ADDRESS)) {
        iaqSensor.begin(BME680_ADDRESS, Wire);

        if (iaqSensor.status != BSEC_OK || iaqSensor.sensor.status != BME68X_OK) {
            LOG_WARN("BSEC", "begin() failed (bsec=%d bme68x=%d)",
                     iaqSensor.status, iaqSensor.sensor.status);
        } else {
            // KORAK 2: setConfig - OBVEZNO pred setState in updateSubscription!
            // Brez tega: status=14 (BSEC_E_CONFIG_FAIL)
            if (!iaqSensor.setConfig(bsec_config_iaq)) {
                LOG_WARN("BSEC", "setConfig failed (status=%d)", iaqSensor.status);
            } else {
                // KORAK 3: naloži shranjeno kalibracijo
                loadBsecState();

                // KORAK 4: registriraj callback
                iaqSensor.attachCallback(bsecDataCallback);

                // KORAK 5: naroči se na izhode
                if (!iaqSensor.updateSubscription(
                    bsecSensorList,
                    sizeof(bsecSensorList) / sizeof(bsecSensorList[0]),
                    BSEC_SAMPLE_RATE_LP
                )) {
                    LOG_WARN("BSEC", "updateSubscription failed (status=%d)",
                             iaqSensor.status);
                } else {
                    bme680Present = true;
                    bme680ErrorCount = 0;
                    sensorData.err &= ~ERR_BME680;
                    LOG_INFO("BSEC", "OK v%d.%d.%d.%d sample_rate=LP",
                             iaqSensor.version.major, iaqSensor.version.minor,
                             iaqSensor.version.major_bugfix, iaqSensor.version.minor_bugfix);
                }
            }
        }
    } else {
        LOG_WARN("BME680", "Not on I2C (0x%02X)", BME680_ADDRESS);
    }

    // --- TCS34725 ---
    if (checkI2CDevice(TCS_ADDRESS)) {
        // Konstruktor sprejema SAMO 2 parametra!
        // &Wire se NIKOLI ne podaja konstruktorju - gre v begin()
        tcs = new Adafruit_TCS34725(TCS34725_INTEGRATIONTIME_154MS, TCS34725_GAIN_4X);
        if (tcs->begin(TCS_ADDRESS, &Wire)) {
            tcsPresent = true;
            tcsErrorCount = 0;
            sensorData.err &= ~ERR_TCS;
            LOG_INFO("TCS34725", "OK");
        } else {
            LOG_WARN("TCS34725", "begin() failed");
            delete tcs; tcs = nullptr;
        }
    } else {
        LOG_WARN("TCS34725", "Not on I2C (0x%02X)", TCS_ADDRESS);
    }

    // --- PIR ---
    pinMode(PIR_PIN, INPUT);
    pirLastState = (digitalRead(PIR_PIN) == HIGH);
    LOG_INFO("PIR", "GPIO%d configured (initial=%s)",
             PIR_PIN, pirLastState ? "HIGH" : "LOW");

    // --- BAT ADC ---
    analogSetAttenuation(ADC_11db);
    LOG_INFO("BAT", "ADC GPIO%d configured", BAT_ADC_PIN);

    LOG_INFO("Sensors", "Init: SHT41=%s BME680+BSEC=%s TCS=%s",
             sht41Present  ? "OK" : "MISSING",
             bme680Present ? "OK" : "MISSING",
             tcsPresent    ? "OK" : "MISSING");

    return sht41Present;  // SHT41 je kritičen
}

// Alias za main.cpp kompatibilnost
bool initSens() { return initSensors(); }

// ============================================================
// LOČENE FUNKCIJE ZA 3-CONSKO ARHITEKTURO (graph_update.md)
// ============================================================

// --- readSHT41() — samo SHT41: temp + hum (klic v 3-min glavni zanki) ---
void readSHT41() {
    if (sht41Present && checkI2CDevice(SHT41_ADDRESS)) {
        float rawT, rawH;
        uint16_t err = sht41->measureHighPrecision(rawT, rawH);
        if (!err && rawT > TEMP_MIN && rawT < TEMP_MAX &&
            rawH >= HUM_MIN && rawH <= HUM_MAX) {
            sht41ErrorCount = 0;
            sensorData.err &= ~ERR_SHT41;
            sensorData.temp = rawT + settings.tempOffset;
            sensorData.hum  = rawH + settings.humOffset;
            newSensorData   = true;
            LOG_INFO("SHT41", "T=%.2f (raw=%.2f off=%.1f) H=%.1f (raw=%.1f off=%.1f)",
                     sensorData.temp, rawT, settings.tempOffset,
                     sensorData.hum,  rawH, settings.humOffset);
        } else {
            sht41ErrorCount++;
            sensorData.err |= ERR_SHT41;
            LOG_WARN("SHT41", "Read failed (err=%u count=%d/%d)",
                     err, sht41ErrorCount, SENSOR_RETRY_COUNT);
            if (sht41ErrorCount >= SENSOR_RETRY_COUNT) {
                sht41Present = false; sht41ErrorCount = 0;
                resetI2CBus();
                LOG_WARN("SHT41", "Marked absent");
            }
        }
    } else if (sht41Present) {
        sht41ErrorCount++;
        sensorData.err |= ERR_SHT41;
        if (sht41ErrorCount >= SENSOR_RETRY_COUNT) {
            sht41Present = false; sht41ErrorCount = 0;
            resetI2CBus();
        }
    }
}

// --- readTCS() — samo TCS34725: lux + cct + rgb (klic v 3-min glavni zanki) ---
void readTCS() {
    if (tcsPresent && checkI2CDevice(TCS_ADDRESS)) {
        uint16_t r, g, b, c;
        tcs->getRawData(&r, &g, &b, &c);
        float lux    = tcs->calculateLux(r, g, b);
        uint16_t cct = tcs->calculateColorTemperature_dn40(r, g, b, c);

        if (lux >= LUX_MIN && lux < LUX_MAX) {
            tcsErrorCount = 0;
            sensorData.err &= ~ERR_TCS;
            sensorData.lux = lux + settings.luxOffset;
            sensorData.cct = cct;
            sensorData.r = r; sensorData.g = g; sensorData.b = b;
            LOG_INFO("TCS", "lux=%.1f cct=%uK", lux, cct);
        } else {
            tcsErrorCount++;
            sensorData.err |= ERR_TCS;
            if (tcsErrorCount >= SENSOR_RETRY_COUNT) {
                tcsPresent = false; tcsErrorCount = 0;
            }
        }
    } else if (tcsPresent) {
        tcsErrorCount++;
        sensorData.err |= ERR_TCS;
        if (tcsErrorCount >= SENSOR_RETRY_COUNT) {
            tcsPresent = false; tcsErrorCount = 0;
        }
    }
}

// --- readPIR() — samo PIR: edge detection, motionCount++ (klic v 1s hitri zanki) ---
// POZOR: Ne logi vsak klic (kliče se vsako sekundo) — samo ob eventih.
void readPIR() {
    bool pirCurrent = (digitalRead(PIR_PIN) == HIGH);

    if (pirCurrent && !pirLastState) {
        // RISING EDGE - novo zaznano gibanje
        sensorData.motion = true;
        sensorData.motionCount++;
        lastMotionMs = millis();

        if (sensorData.cameraReady) {
            startMotionRecording();
            LOG_INFO("PIR", "MOTION detected -> recording triggered (count=%u)",
                     sensorData.motionCount);
        } else {
            LOG_INFO("PIR", "MOTION detected (count=%u, camera not ready)",
                     sensorData.motionCount);
        }

    } else if (pirCurrent && pirLastState) {
        // Drži HIGH - gibanje se nadaljuje, osvezi timestamp
        lastMotionMs = millis();

    } else if (!pirCurrent && pirLastState) {
        // FALLING EDGE — gibanje zaključeno
        completedMotionTime = time(nullptr);
        LOG_DEBUG("PIR", "Motion completed at %lu", (unsigned long)completedMotionTime);
        logMotionEvent(completedMotionTime);
    }

    pirLastState = pirCurrent;

    // Briši motion flag po 30s brez gibanja (fiksno — settings.readIntervalSec je deprecated)
    if (!pirCurrent && millis() - lastMotionMs > 30000UL) {
        sensorData.motion = false;
    }
}

// ============================================================
// BRANJE SENZORJEV — ohranjeno kot wrapper za kompatibilnost
// V novi arhitekturi main.cpp kliče readSHT41()+readTCS()+readPIR() ločeno.
// runSens() / readSensors() sta ohranjena za morebitno staro kodo.
// ============================================================
void readSensors() {
    readSHT41();
    readTCS();
    readPIR();
}

// Alias za main.cpp kompatibilnost
void runSens() { readSensors(); }

// ============================================================
// BATERIJA
// ============================================================
void readBattery() {
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) {
        sum += analogRead(BAT_ADC_PIN);
        delayMicroseconds(100);
    }
    float adcV = (sum / 16.0f) * (3.3f / 4095.0f);
    float vbat = adcV * BAT_VOLTAGE_DIVIDER;
    vbat = constrain(vbat, 3.0f, 4.2f);
    sensorData.bat    = vbat;
    sensorData.batPct = (uint8_t)constrain(
        (int)((vbat - 3.0f) / 1.2f * 100.0f), 0, 100);
    LOG_DEBUG("BAT", "%.3fV %u%%", vbat, sensorData.batPct);
}

// ============================================================
// PERIODICNI SENSOR CHECK - reconnect (vsakih 10 min)
// ============================================================
void performPeriodicSensorCheck() {
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck < 600000UL) return;
    lastCheck = millis();

    // SHT41
    if (!sht41Present && checkI2CDevice(SHT41_ADDRESS)) {
        if (sht41) { delete sht41; sht41 = nullptr; }
        sht41 = new SensirionI2cSht4x();
        sht41->begin(Wire, SHT41_ADDRESS); sht41->softReset(); delay(10);
        float t, h;
        if (!sht41->measureHighPrecision(t, h) && t > TEMP_MIN && t < TEMP_MAX) {
            sht41Present = true; sht41ErrorCount = 0;
            sensorData.err &= ~ERR_SHT41;
            LOG_INFO("SHT41", "Reconnected T=%.1f H=%.1f", t, h);
        } else {
            LOG_WARN("SHT41", "Reconnect failed"); delete sht41; sht41 = nullptr;
        }
    }

    // BME680 + BSEC2 reconnect
    // Enak vrstni red kot initSensors: begin → setConfig → loadBsecState → attachCallback → updateSubscription
    if (!bme680Present && checkI2CDevice(BME680_ADDRESS)) {
        iaqSensor.begin(BME680_ADDRESS, Wire);
        if (iaqSensor.status == BSEC_OK && iaqSensor.sensor.status == BME68X_OK) {
            if (iaqSensor.setConfig(bsec_config_iaq)) {
                loadBsecState();
                iaqSensor.attachCallback(bsecDataCallback);
                if (iaqSensor.updateSubscription(
                    bsecSensorList,
                    sizeof(bsecSensorList) / sizeof(bsecSensorList[0]),
                    BSEC_SAMPLE_RATE_LP
                )) {
                    bme680Present = true; bme680ErrorCount = 0;
                    sensorData.err &= ~ERR_BME680;
                    LOG_INFO("BSEC", "BME680 reconnected, state restored");
                }
            }
        } else {
            LOG_WARN("BSEC", "BME680 reconnect failed (bsec=%d)", iaqSensor.status);
        }
    }

    // TCS34725
    if (!tcsPresent && checkI2CDevice(TCS_ADDRESS)) {
        if (tcs) { delete tcs; tcs = nullptr; }
        tcs = new Adafruit_TCS34725(TCS34725_INTEGRATIONTIME_154MS, TCS34725_GAIN_4X);
        if (tcs->begin(TCS_ADDRESS, &Wire)) {
            tcsPresent = true; tcsErrorCount = 0;
            sensorData.err &= ~ERR_TCS;
            LOG_INFO("TCS34725", "Reconnected");
        } else {
            LOG_WARN("TCS34725", "Reconnect failed"); delete tcs; tcs = nullptr;
        }
    }

    LOG_INFO("Sensors", "PeriodicCheck: SHT41=%s BME680=%s TCS=%s",
             sht41Present  ? "ok" : "absent",
             bme680Present ? "ok" : "absent",
             tcsPresent    ? "ok" : "absent");
}

// ============================================================
// PERIODICNI I2C RESET (vsakih 30 min, samo ob napakah)
// ============================================================
void performPeriodicI2CReset() {
    static unsigned long lastReset = 0;
    if (millis() - lastReset < 1800000UL) return;
    lastReset = millis();
    if (sensorData.err & (ERR_SHT41 | ERR_BME680 | ERR_TCS)) {
        LOG_INFO("I2C", "Periodic reset (active errors)");
        resetI2CBus();
    }
}

// ============================================================
// LOG MOTION EVENT — zapis FALLING EDGE timestampa na SD
// Klic: readPIR() ob FALLING EDGE (ko pirCurrent=LOW, pirLastState=HIGH)
// Format: /motion/YYYY-MM-DD.csv, vsaka vrstica = Unix timestamp
// Pogoji za zapis: sdPresent && timeSynced && sdMutex dostopen
// ============================================================
void logMotionEvent(time_t ts) {
    if (!sdPresent)   return;
    if (!timeSynced)  return;  // brez NTP čas nima smisla

    char path[32];
    struct tm* ti = localtime(&ts);
    strftime(path, sizeof(path), "/motion/%Y-%m-%d.csv", ti);

    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        LOG_WARN("PIR", "logMotionEvent: SD mutex timeout");
        return;
    }

    // Ustvari /motion/ direktorij če ne obstaja
    if (!SD.exists("/motion")) SD.mkdir("/motion");

    File f = SD.open(path, FILE_APPEND);
    if (f) {
        f.println((unsigned long)ts);
        f.close();
        LOG_DEBUG("PIR", "logMotionEvent: %lu → %s", (unsigned long)ts, path);
    } else {
        LOG_WARN("PIR", "logMotionEvent: cannot open %s", path);
    }

    xSemaphoreGive(sdMutex);
}

// ============================================================
// RESET (ob kritični napaki)
// ============================================================
void resetSensors() {
    LOG_WARN("Sensors", "Full reset");
    resetI2CBus();
    if (sht41) { delete sht41; sht41 = nullptr; }
    if (tcs)   { delete tcs;   tcs   = nullptr; }
    sht41Present = bme680Present = tcsPresent = false;
    sht41ErrorCount = bme680ErrorCount = tcsErrorCount = 0;
    pirLastState = false;
    newSensorData = false;
    initSensors();
}

// ============================================================
// BSEC2 POLLING — klic iz 1s hitre zanke (graph_update.md)
// ============================================================
//
// runBsec(): BSEC2 SAMPLE_RATE_LP vzorči vsakih ~3s. Za pravilen callback
//   mora iaqSensor.run() biti klican pogosto (vsaj vsako sekundo).
//   run() je neblokirajoča: takoj vrne false če ni čas za meritev.
//   Ko pride čas za vzorčenje (vsake ~3s), pokliče bsecDataCallback().
//
void runBsec() {
    if (!bme680Present) return;

    if (!iaqSensor.run()) {
        // run() vrne false: ali ni novih podatkov (normalno za LP mode)
        // ali pa je prišlo do napake
        if (iaqSensor.status < BSEC_OK || iaqSensor.sensor.status < BME68X_OK) {
            bme680ErrorCount++;
            sensorData.err |= ERR_BME680;
            LOG_WARN("BSEC", "Error (bsec=%d bme68x=%d count=%d/%d)",
                     iaqSensor.status, iaqSensor.sensor.status,
                     bme680ErrorCount, SENSOR_RETRY_COUNT);
            if (bme680ErrorCount >= SENSOR_RETRY_COUNT) {
                bme680Present = false;
                bme680ErrorCount = 0;
                LOG_WARN("BSEC", "BME680 marked absent");
            }
        }
        // else: run()==false brez napake = normalno, čaka na naslednji LP interval
    }
    // else: run()==true → bsecDataCallback je bil poklican, sensorData je posodobljen
}

// Alias za kompatibilnost (staro ime)
void runBsecLoop() { runBsec(); }
