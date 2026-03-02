// sens.cpp - Sensor module implementation for vent_SEW
//
// Senzorji na I2C_Sensors (Wire1, IO33/IO34):
//   SHT41    (0x44) - temperatura, vlažnost
//   BME680   (0x76) - tlak, IAQ, eCO2, breathVOC (via BSEC 1.4.9.2)
//   TCS34725 (0x29) - lux, CCT, RGB
//   PIR      (IO35) - gibanje → ob detekciji kliče startMotionRecording()
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
//   - resetI2CBus(): 9x SCL toggle + Wire1.end/begin
//   - error counter: absent po SENSOR_RETRY_COUNT napakah
//   - performPeriodicSensorCheck(): reconnect vsakih 10 min
//   - performPeriodicI2CReset(): preventivni reset vsakih 30 min
//
// Baterija:
//   - ADC IO5, delilnik 200K/100K → faktor x3, povprečje 16 vzorcev
//
// POPRAVKI (2026-03-02):
//   - FIX: TCS34725 konstruktor popravljen.
//     Adafruit_TCS34725 sprejema SAMO 2 parametra (integrationTime, gain).
//     &Wire1 se NE sme podati konstruktorju - gre v begin().
//     NAPAČNO: new Adafruit_TCS34725(TIME, GAIN, &Wire1)  ← ne obstaja 3-param konstruktor
//     PRAVILNO: new Adafruit_TCS34725(TIME, GAIN)
//               tcs->begin(TCS_ADDRESS, &Wire1)            ← Wire1 gre sem
//     Popravek apliciran na 2 mestih: initSensors() + performPeriodicSensorCheck()
//
// POPRAVKI (2026-03-02 v2):
//   - FIX: #include <SensirionI2CSht4x.h> → <SensirionI2cSht4x.h>
//     Pravi header iz library.properties: SensirionI2cSht4x.h (malo 'c', malo 'x')
//     Velika 'C' v SensirionI2CSht4x.h povzroča napako pri kompilaciji na
//     case-sensitive datotečnih sistemih (Linux/PlatformIO build sistem).

#include "sens.h"
#include "cam.h"       // startMotionRecording()
#include "globals.h"
#include "logging.h"
#include <Wire.h>
#include <SensirionI2cSht4x.h>  // FIX (2026-03-02 v2): pravilno ime - malo 'c' in malo 'x'
                                  // NAPACNO bilo: SensirionI2CSht4x.h (velika 'C')
#include <bsec.h>                   // BSEC-Arduino-library v1.4.9.2
#include <Adafruit_TCS34725.h>
#include <Preferences.h>

// ============================================================
// Senzorski objekti
// ============================================================
static SensirionI2cSht4x* sht41 = nullptr;  // FIX: tip usklajen s popravljenim headerjem
static Bsec               iaqSensor;   // BSEC objekt (ne pointer - zahteva lib)
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
// BSEC subscriptions
// ============================================================
static const bsec_virtual_sensor_t bsecSensorList[] = {
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
// I2C bus init (Wire1 - zunanji senzorji)
// ============================================================
void initI2CBus() {
    Wire1.begin(I2C_SENS_SDA, I2C_SENS_SCL);
    Wire1.setClock(I2C_CLOCK_SPEED);
    Wire1.setTimeout(I2C_TIMEOUT_MS);
    LOG_INFO("I2C", "Wire1 init %dHz SDA=%d SCL=%d",
             I2C_CLOCK_SPEED, I2C_SENS_SDA, I2C_SENS_SCL);
}

bool checkI2CDevice(uint8_t address) {
    Wire1.beginTransmission(address);
    return (Wire1.endTransmission() == 0);
}

void resetI2CBus() {
    // Standard I2C recovery: 9x SCL toggle
    pinMode(I2C_SENS_SCL, OUTPUT);
    pinMode(I2C_SENS_SDA, INPUT);
    for (int i = 0; i < 9; i++) {
        digitalWrite(I2C_SENS_SCL, LOW);  delayMicroseconds(10);
        digitalWrite(I2C_SENS_SCL, HIGH); delayMicroseconds(10);
    }
    if (digitalRead(I2C_SENS_SDA) == LOW)
        LOG_ERROR("I2C", "SDA stuck low after recovery!");
    Wire1.end();
    delay(10);
    Wire1.begin(I2C_SENS_SDA, I2C_SENS_SCL);
    Wire1.setClock(I2C_CLOCK_SPEED);
    Wire1.setTimeout(I2C_TIMEOUT_MS);
    LOG_INFO("I2C", "Bus reset complete");
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

    iaqSensor.setState(stateBlob);
    if (iaqSensor.bsecStatus != BSEC_OK) {
        LOG_WARN("BSEC", "setState failed (status=%d) - starting fresh", iaqSensor.bsecStatus);
    } else {
        LOG_INFO("BSEC", "State loaded from NVS - calibration continues");
    }
}

static void saveBsecState() {
    uint8_t stateBlob[BSEC_MAX_STATE_BLOB_SIZE];
    iaqSensor.getState(stateBlob);

    if (iaqSensor.bsecStatus != BSEC_OK) {
        LOG_WARN("BSEC", "getState failed (status=%d) - not saving", iaqSensor.bsecStatus);
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
        if (iaqSensor.iaqAccuracy >= 3) {
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
// INICIALIZACIJA SENZORJEV
// ============================================================
bool initSensors() {
    initI2CBus();

    sensorData.err |= (ERR_SHT41 | ERR_BME680 | ERR_TCS);
    sht41Present = bme680Present = tcsPresent = false;

    // --- SHT41 ---
    if (checkI2CDevice(SHT41_ADDRESS)) {
        sht41 = new SensirionI2cSht4x();  // FIX: tip usklajen s popravljenim headerjem
        sht41->begin(Wire1);
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

    // --- BME680 + BSEC ---
    if (checkI2CDevice(BME680_ADDRESS)) {
        iaqSensor.begin(BME680_ADDRESS, Wire1);

        if (iaqSensor.bsecStatus != BSEC_OK || iaqSensor.bme68xStatus != BME68X_OK) {
            LOG_WARN("BSEC", "begin() failed (bsec=%d bme68x=%d)",
                     iaqSensor.bsecStatus, iaqSensor.bme68xStatus);
        } else {
            loadBsecState();

            iaqSensor.updateSubscription(
                bsecSensorList,
                sizeof(bsecSensorList) / sizeof(bsecSensorList[0]),
                BSEC_SAMPLE_RATE_LP
            );

            if (iaqSensor.bsecStatus != BSEC_OK) {
                LOG_WARN("BSEC", "updateSubscription failed (status=%d)", iaqSensor.bsecStatus);
            } else {
                bme680Present = true;
                bme680ErrorCount = 0;
                sensorData.err &= ~ERR_BME680;
                LOG_INFO("BSEC", "OK v%d.%d.%d.%d sample_rate=LP",
                         iaqSensor.version.major, iaqSensor.version.minor,
                         iaqSensor.version.major_bugfix, iaqSensor.version.minor_bugfix);
            }
        }
    } else {
        LOG_WARN("BME680", "Not on I2C (0x%02X)", BME680_ADDRESS);
    }

    // --- TCS34725 ---
    if (checkI2CDevice(TCS_ADDRESS)) {
        // FIX (2026-03-02): Konstruktor sprejema SAMO 2 parametra!
        // &Wire1 se NIKOLI ne podaja konstruktorju - Adafruit_TCS34725 ga ne sprejema.
        // Wire1 gre v begin() kot 2. parameter.
        // NAPAČNO (prej): new Adafruit_TCS34725(TIME, GAIN, &Wire1)
        // PRAVILNO (zdaj): new Adafruit_TCS34725(TIME, GAIN)
        tcs = new Adafruit_TCS34725(TCS34725_INTEGRATIONTIME_154MS, TCS34725_GAIN_4X);
        if (tcs->begin(TCS_ADDRESS, &Wire1)) {
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
// BRANJE SENZORJEV
// ============================================================
void readSensors() {
    bool sht41Ok = false;

    // --- SHT41 ---
    if (sht41Present && checkI2CDevice(SHT41_ADDRESS)) {
        float rawT, rawH;
        uint16_t err = sht41->measureHighPrecision(rawT, rawH);
        if (!err && rawT > TEMP_MIN && rawT < TEMP_MAX &&
            rawH >= HUM_MIN && rawH <= HUM_MAX) {
            sht41ErrorCount = 0;
            sensorData.err &= ~ERR_SHT41;
            sensorData.temp = rawT + settings.tempOffset;
            sensorData.hum  = rawH + settings.humOffset;
            sht41Ok = true;
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

    // --- BME680 + BSEC ---
    if (bme680Present) {
        if (iaqSensor.run()) {
            bme680ErrorCount = 0;
            sensorData.err &= ~ERR_BME680;

            sensorData.press     = iaqSensor.rawPressure / 100.0f + settings.pressOffset;
            sensorData.iaq       = (uint16_t)iaqSensor.iaq;
            sensorData.iaqAccuracy = iaqSensor.iaqAccuracy;
            sensorData.staticIaq = (uint16_t)iaqSensor.staticIaq;
            sensorData.eCO2      = iaqSensor.co2Equivalent;
            sensorData.breathVOC = iaqSensor.breathVocEquivalent;

            LOG_INFO("BSEC", "P=%.1fhPa IAQ=%u(acc=%d) sIAQ=%u eCO2=%.0f bVOC=%.2f",
                     sensorData.press,
                     sensorData.iaq, sensorData.iaqAccuracy,
                     sensorData.staticIaq,
                     sensorData.eCO2, sensorData.breathVOC);

            updateBsecState();

        } else if (iaqSensor.bsecStatus < BSEC_OK || iaqSensor.bme68xStatus < BME68X_OK) {
            bme680ErrorCount++;
            sensorData.err |= ERR_BME680;
            LOG_WARN("BSEC", "Error (bsec=%d bme68x=%d count=%d/%d)",
                     iaqSensor.bsecStatus, iaqSensor.bme68xStatus,
                     bme680ErrorCount, SENSOR_RETRY_COUNT);
            if (bme680ErrorCount >= SENSOR_RETRY_COUNT) {
                bme680Present = false; bme680ErrorCount = 0;
                LOG_WARN("BSEC", "BME680 marked absent");
            }
        }
        // else: run()==false brez napake = normalno, ni novih podatkov
    }

    // --- TCS34725 ---
    if (tcsPresent && checkI2CDevice(TCS_ADDRESS)) {
        uint16_t r, g, b, c;
        tcs->getRawData(&r, &g, &b, &c);
        float lux = tcs->calculateLux(r, g, b);
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

    // --- PIR ---
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
        // Drži HIGH - gibanje se nadaljuje
        lastMotionMs = millis();

    } else if (!pirCurrent && pirLastState) {
        // FALLING EDGE - gibanje se je ustavilo
        LOG_DEBUG("PIR", "Motion ended");
    }

    pirLastState = pirCurrent;

    // Briši motion flag po readInterval brez gibanja
    if (!pirCurrent &&
        millis() - lastMotionMs > (unsigned long)settings.readIntervalSec * 1000UL) {
        sensorData.motion = false;
    }

    // newSensorData flag - postavi na true ob uspešnem branju
    if (sht41Ok) {
        newSensorData = true;
    }
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
        sht41 = new SensirionI2cSht4x();  // FIX: tip usklajen s popravljenim headerjem
        sht41->begin(Wire1); sht41->softReset(); delay(10);
        float t, h;
        if (!sht41->measureHighPrecision(t, h) && t > TEMP_MIN && t < TEMP_MAX) {
            sht41Present = true; sht41ErrorCount = 0;
            sensorData.err &= ~ERR_SHT41;
            LOG_INFO("SHT41", "Reconnected T=%.1f H=%.1f", t, h);
        } else {
            LOG_WARN("SHT41", "Reconnect failed"); delete sht41; sht41 = nullptr;
        }
    }

    // BME680 + BSEC
    if (!bme680Present && checkI2CDevice(BME680_ADDRESS)) {
        iaqSensor.begin(BME680_ADDRESS, Wire1);
        if (iaqSensor.bsecStatus == BSEC_OK && iaqSensor.bme68xStatus == BME68X_OK) {
            loadBsecState();
            iaqSensor.updateSubscription(
                bsecSensorList,
                sizeof(bsecSensorList) / sizeof(bsecSensorList[0]),
                BSEC_SAMPLE_RATE_LP
            );
            if (iaqSensor.bsecStatus == BSEC_OK) {
                bme680Present = true; bme680ErrorCount = 0;
                sensorData.err &= ~ERR_BME680;
                LOG_INFO("BSEC", "BME680 reconnected, state restored");
            }
        } else {
            LOG_WARN("BSEC", "BME680 reconnect failed (bsec=%d)", iaqSensor.bsecStatus);
        }
    }

    // TCS34725
    if (!tcsPresent && checkI2CDevice(TCS_ADDRESS)) {
        if (tcs) { delete tcs; tcs = nullptr; }
        // FIX (2026-03-02): Konstruktor brez &Wire1 (enako kot initSensors zgoraj)
        tcs = new Adafruit_TCS34725(TCS34725_INTEGRATIONTIME_154MS, TCS34725_GAIN_4X);
        if (tcs->begin(TCS_ADDRESS, &Wire1)) {
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
