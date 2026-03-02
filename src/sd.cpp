// sd.cpp - SD card module implementation for vent_SEW
//
// Knjižnica: SD (SPI način) - ker SD deli SPI bus z LCD (IO38/IO39)
// Plošča:    Waveshare ESP32-S3-Touch-LCD-2
// Pini:      MOSI=IO38, SCLK=IO39, MISO=IO40, CS=IO41 (config.h)
//
// POZOR - skupni SPI bus z LCD:
//   - Vse SD operacije morajo pridobiti sdMutex pred dostopom
//   - LVGL ne sme pisati na zaslon med SD operacijo
//   - Mutex timeout: 200ms za flush, 500ms za init/cleanup
//
// SPI BUS DELJENJE:
//   LCD gonilnik (Display_ST7789.cpp) inicializira SPI bus z SPI.begin().
//   SD.begin() mora dobiti referenco na isti SPIClass objekt - sicer
//   SD knjižnica inicializira nov (2.) SPI bus, kar povzroči konflikte.
//   Rešitev: SD.begin(SD_CS_PIN, SPI) - SPI je globalni SPIClass objekt
//   iz Arduino SPI.h, ki ga je LCD gonilnik že inicializiral.
//
// Datoteke na SD:
//   sew_YYYY-MM-DD.csv   - senzorske meritve (vsak DATA_SEND_INTERVAL)
//   log_YYYY-MM-DD.txt   - sistemski logi (logging.cpp)
//
// POPRAVKI (2026-02-28):
//   - sensorData.voc -> sensorData.breathVOC (SensorData nima "voc" polja)
//   - CSV header popravljen: voc -> breath_voc, dodani iaq, siaq, eco2 stolpci
//
// POPRAVKI (2026-02-28 v2):
//   - SD.begin(SD_CS_PIN) -> SD.begin(SD_CS_PIN, SPI)
//     Ekspliciten SPI parameter zagotovi, da SD uporablja isti SPI bus
//     kot LCD gonilnik in ne inicializira svojega SPI busa.
//   - listFiles(): popravljen iteracijski vzorec (shrani podatke pred close())

#include "sd.h"
#include "globals.h"
#include "logging.h"
#include <SPI.h>

// ============================================================
// Inicializacija
// ============================================================
bool initSD() {
    // Skupni SPI bus z LCD - mutex mora obstajati
    if (sdMutex == NULL) {
        Serial.println("SD: ERROR - sdMutex not initialized!");
        return false;
    }

    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        Serial.println("SD: Cannot acquire SPI mutex for init");
        return false;
    }

    // FIX: SD.begin(SD_CS_PIN, SPI) - eksplicitni SPI objekt!
    // LCD gonilnik je ze inicializiral SPI bus (MOSI=IO38, SCLK=IO39).
    // SD.begin() z eksplicitnim SPI parametrom zagotovi, da SD
    // ne inicializira svojega SPI busa, ampak se prikljuci na obstojecega.
    // Brez tega parametra bi SD knjiznica klicala SPI.begin() z defaultnimi
    // pini, kar bi povzrocilo konflikt na skupnem busu.
    bool ok = SD.begin(SD_CS_PIN, SPI);

    if (!ok) {
        xSemaphoreGive(sdMutex);
        Serial.println("SD: Initialization failed - no card or SPI error");
        sensorData.err |= ERR_SD;
        return false;
    }

    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
        xSemaphoreGive(sdMutex);
        Serial.println("SD: No card detected");
        sensorData.err |= ERR_SD;
        return false;
    }

    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    uint64_t cardFree = (SD.totalBytes() - SD.usedBytes()) / (1024 * 1024);

    xSemaphoreGive(sdMutex);

    sensorData.err &= ~ERR_SD;
    Serial.printf("SD: OK - type=%d size=%lluMB free=%lluMB\n",
                  cardType, cardSize, cardFree);
    return true;
}

// ============================================================
// Pomozne funkcije
// ============================================================

static uint32_t getTimestamp() {
    return (timeSynced && myTZ.now() > 1577836800UL)
           ? (uint32_t)myTZ.now()
           : (uint32_t)(millis() / 1000);
}

static String getDatetime() {
    return (timeSynced && myTZ.now() > 1577836800UL)
           ? myTZ.dateTime("Y-m-d H:i:s")
           : String("notime");
}

static String makeFilename(const char* prefix, const char* ext) {
    if (timeSynced && myTZ.now() > 1577836800UL) {
        return "/" + String(prefix) + myTZ.dateTime("Y-m-d") + ext;
    }
    return "/" + String(prefix) + "nodate" + ext;
}

// Ustvari datoteko s CSV glavo ce ne obstaja se
static bool ensureCSV(const String& filename, const char* header) {
    if (!SD.exists(filename.c_str())) {
        File f = SD.open(filename.c_str(), FILE_WRITE);
        if (!f) {
            LOG_ERROR("SD", "Cannot create: %s", filename.c_str());
            return false;
        }
        f.println(header);
        f.close();
        LOG_INFO("SD", "Created: %s", filename.c_str());
    }
    return true;
}

// ============================================================
// saveSDData() - shrani senzorske meritve
//
// Klic: po vsakem uspesnem posiljanju na REW (ali ob vsakem
//       branju senzorjev, odvisno od nastavitev)
//
// Format CSV (skladen s SensorData polji):
//   timestamp, datetime, unit_id,
//   temp, hum, press,
//   iaq, siaq, iaq_acc, eco2, breath_voc,   <- BSEC polja
//   lux, cct,
//   motion, motion_count,
//   bat_v, bat_pct,
//   err, http_code
// ============================================================
void saveSDData() {
    if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        LOG_WARN("SD", "saveSDData: cannot acquire mutex");
        return;
    }

    String filename = makeFilename("sew_", ".csv");

    // Header usklajen s SensorData polji
    static const char* HEADER =
        "timestamp,datetime,unit_id,"
        "temp,hum,press,"
        "iaq,siaq,iaq_acc,eco2,breath_voc,"
        "lux,cct,"
        "motion,motion_count,"
        "bat_v,bat_pct,"
        "err,http_code";

    if (!ensureCSV(filename, HEADER)) {
        if (sdMutex) xSemaphoreGive(sdMutex);
        return;
    }

    File f = SD.open(filename.c_str(), FILE_APPEND);
    if (!f) {
        LOG_ERROR("SD", "Cannot open for append: %s", filename.c_str());
        if (sdMutex) xSemaphoreGive(sdMutex);
        sensorData.err |= ERR_SD;
        return;
    }

    char line[320];
    snprintf(line, sizeof(line),
             "%u,%s,%s,"
             "%.2f,%.2f,%.2f,"
             "%u,%u,%u,%.0f,%.2f,"
             "%.1f,%u,"
             "%d,%u,"
             "%.3f,%u,"
             "%u,%d",
             getTimestamp(),
             getDatetime().c_str(),
             sensorData.unitId,
             // temp, hum, press
             sensorData.temp,
             sensorData.hum,
             sensorData.press,
             // BSEC
             (unsigned)sensorData.iaq,
             (unsigned)sensorData.staticIaq,
             (unsigned)sensorData.iaqAccuracy,
             sensorData.eCO2,
             sensorData.breathVOC,
             // lux, cct
             sensorData.lux,
             (unsigned)sensorData.cct,
             // gibanje
             sensorData.motion ? 1 : 0,
             (unsigned)sensorData.motionCount,
             // baterija
             sensorData.bat,
             (unsigned)sensorData.batPct,
             // stanje
             (unsigned)sensorData.err,
             sensorData.lastHttpCode);

    f.println(line);
    f.close();
    if (sdMutex) xSemaphoreGive(sdMutex);

    sensorData.err &= ~ERR_SD;
    LOG_DEBUG("SD", "Data saved: %s T=%.1f H=%.1f P=%.1f IAQ=%u lux=%.0f",
              filename.c_str(),
              sensorData.temp, sensorData.hum,
              sensorData.press, sensorData.iaq, sensorData.lux);
}

// ============================================================
// readFile() - preberi datoteko (za web vmesnik /sd-file)
// ============================================================
String readFile(const char* path) {
    if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        LOG_WARN("SD", "readFile: cannot acquire mutex");
        return "";
    }

    File f = SD.open(path, FILE_READ);
    if (!f) {
        if (sdMutex) xSemaphoreGive(sdMutex);
        LOG_WARN("SD", "File open failed: %s", path);
        return "";
    }

    String s;
    s.reserve(f.size());
    while (f.available()) s += (char)f.read();
    f.close();
    if (sdMutex) xSemaphoreGive(sdMutex);

    return s;
}

// ============================================================
// listFiles() - vrne JSON seznam datotek (za web vmesnik)
// ============================================================
String listFiles() {
    if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        LOG_WARN("SD", "listFiles: cannot acquire mutex");
        return "[]";
    }

    File root = SD.open("/");
    if (!root) {
        if (sdMutex) xSemaphoreGive(sdMutex);
        LOG_ERROR("SD", "listFiles: cannot open root");
        return "[]";
    }

    String json = "[";
    bool first = true;

    File entry = root.openNextFile();
    while (entry) {
        // FIX: shrani podatke pred close()
        String name = String(entry.name());
        size_t sz = entry.size();
        bool isDir = entry.isDirectory();
        entry.close();

        if (!isDir) {
            if (!first) json += ",";
            json += "{\"name\":\"";
            json += name;
            json += "\",\"size\":";
            json += sz;
            json += "}";
            first = false;
        }

        entry = root.openNextFile();
    }
    root.close();
    if (sdMutex) xSemaphoreGive(sdMutex);

    json += "]";
    return json;
}
