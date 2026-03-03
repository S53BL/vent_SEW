// sd_card.cpp - SD card module implementation for vent_SEW
//
// Knjižnica: SD (SPI način) - ker SD deli SPI bus z LCD (IO38/IO39)
// Plošča:    Waveshare ESP32-S3-Touch-LCD-2
// Pini:      MOSI=IO38, SCLK=IO39, MISO=IO40, CS=IO41 (config.h)
//
// POZOR - skupni SPI bus z LCD:
//   - LCD gonilnik (Display_ST7789.cpp) inicializira FSPI bus prek objekta
//     LCDspi(FSPI) z LCDspi.begin(SCLK=39, MISO=-1, MOSI=38).
//   - SD.begin() MORA dobiti isti SPIClass objekt (LCDspi), sicer SD
//     knjižnica ne ve za pravilen SPI bus in init ne uspe.
//   - Ker LCD inicializira bus brez MISO (-1), moramo MISO (IO40) dodati
//     ročno pred SD.begin() z LCDspi.begin(SCLK, MISO, MOSI) klicem.
//   - Vse SD operacije morajo pridobiti sdMutex pred dostopom.
//
// POPRAVKI (2026-03-03):
//   - KRITIČNA NAPAKA ODPRAVLJENA: SD.begin(SD_CS_PIN, SPI) → SD.begin(SD_CS_PIN, LCDspi)
//     Razlog: LCD gonilnik uporablja lokalni SPIClass LCDspi(FSPI), ne globalni SPI.
//     SD.begin() z globalnim SPI objektom ne dobi veljavnega SPI busa → init fail.
//   - MISO pin dodan: LCDspi.begin() ob LCD init ne konfigurira MISO (-1).
//     SD kartica nujno potrebuje MISO (IO40) → pred SD.begin() kličemo
//     LCDspi.begin(SD_SCLK_PIN, SD_MISO_PIN, SD_MOSI_PIN) da MISO doda na bus.
//
// Datoteke na SD:
//   sew_YYYY-MM-DD.csv   - senzorske meritve (vsak DATA_SEND_INTERVAL)
//   log_YYYY-MM-DD.txt   - sistemski logi (logging.cpp)

#include "sd_card.h"
#include "globals.h"
#include "logging.h"
#include <SPI.h>
#include <SD.h>

// LCDspi je definiran v Display_ST7789.cpp (SPIClass LCDspi(FSPI))
// SD mora dobiti referenco na ta isti objekt!
extern SPIClass LCDspi;

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

    // Konfiguriraj MISO pin na obstoječem LCDspi objektu.
    // LCD gonilnik je klical LCDspi.begin(SCLK=39, MISO=-1, MOSI=38) - brez MISO.
    // SD kartica potrebuje MISO (IO40), zato pokličemo begin() ponovno z MISO pinom.
    // SPI.begin() na ESP32 Arduino je idempotenten glede že nastavljenih pinov -
    // samo doda/posodobi pine ki so podani, ne resetira busa.
    LCDspi.begin(SD_SCLK_PIN, SD_MISO_PIN, SD_MOSI_PIN);

    // FIX: SD.begin(SD_CS_PIN, LCDspi) - eksplicitni LCDspi objekt!
    // LCD gonilnik je inicializiral FSPI bus prek LCDspi(FSPI), ne prek globalnega SPI.
    // SD.begin() z globalnim SPI ne dobi veljavnega busa → init fail.
    // Z LCDspi referenco SD pravilno uporablja že inicializiran FSPI bus.
    bool ok = SD.begin(SD_CS_PIN, LCDspi);

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
// ============================================================
void saveSDData() {
    if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        LOG_WARN("SD", "saveSDData: cannot acquire mutex");
        return;
    }

    String filename = makeFilename("sew_", ".csv");

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
             sensorData.temp,
             sensorData.hum,
             sensorData.press,
             (unsigned)sensorData.iaq,
             (unsigned)sensorData.staticIaq,
             (unsigned)sensorData.iaqAccuracy,
             sensorData.eCO2,
             sensorData.breathVOC,
             sensorData.lux,
             (unsigned)sensorData.cct,
             sensorData.motion ? 1 : 0,
             (unsigned)sensorData.motionCount,
             sensorData.bat,
             (unsigned)sensorData.batPct,
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
// readFileSD() - preberi datoteko (za web vmesnik /sd-file)
// ============================================================
String readFileSD(const char* path) {
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
