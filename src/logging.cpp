// logging.cpp - Logging system implementation for vent_SEW
//
// RAM buffer (flush samo ko doseže 12 kB - periodični flush odstranjen):
//   logBuffer         - String RAM buffer (extern v globals.h)
//   loggingInitialized - bool flag (extern v globals.h)
//   currentLogFile     - ime aktivne log datoteke (extern v globals.h)
//
// POZOR: logBuffer, loggingInitialized, currentLogFile so tukaj DEFINIRANI
//        (brez "static"!) ker so extern deklarirani v globals.h.
//        globals.cpp jih NE sme definirati (to bi bila dvojna definicija).
//
// Logi se pišejo v /log_YYYY-MM-DD.txt na SD kartici.
// myTZ je definiran v globals.cpp, tukaj samo referenciran.
//
// POPRAVKI (2026-02-28):
//   - cleanupOldLogs(): popravljen SD iteracijski bug
//     entry.close() je bil klican PRED root.openNextFile(), kar je povzročilo
//     napačno iteracijo (File objekt je bil neveljaven pri naslednjem klicu).
//     Rešitev: shranimo ime datoteke pred close(), potem naredimo remove().

#include "logging.h"
#include "globals.h"
#include "sd_card.h"
#include <SD.h>

// ============================================================
// Definicije (BREZ static - extern v globals.h)
// ============================================================
String logBuffer          = "";
bool   loggingInitialized = false;
String currentLogFile     = "";

// ============================================================
// Inicializacija
// ============================================================
void initLogging() {
    logBuffer = "";
    currentLogFile = "";
    loggingInitialized = true;
    LOG_INFO("LOG", "Logging initialized - buffer max %d bytes", LOG_BUFFER_MAX);
}

// ============================================================
// Glavni logEvent - edini entry point
// ============================================================
void logEvent(LogLevel level, const char* tag, const char* format, ...) {
    // 1. Sestavi sporočilo iz format stringa
    char message[256];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    // 2. Level string
    const char* levelStr = "DEBUG";
    switch (level) {
        case LOG_LEVEL_INFO:  levelStr = "INFO";  break;
        case LOG_LEVEL_WARN:  levelStr = "WARN";  break;
        case LOG_LEVEL_ERROR: levelStr = "ERROR"; break;
        default:              levelStr = "DEBUG"; break;
    }

    // 3. Timestamp: Unix čas če NTP sinhroniziran, sicer M<millis>
    char timestamp[32];
    if (timeSynced && myTZ.now() > 1577836800UL) {
        snprintf(timestamp, sizeof(timestamp), "%u", (uint32_t)myTZ.now());
    } else {
        snprintf(timestamp, sizeof(timestamp), "M%lu", millis());
    }

    // 4. Sestavi log linijo: timestamp|unitId|[tag:LEVEL] message
    char logLine[320];
    snprintf(logLine, sizeof(logLine), "%s|%s|[%s:%s] %s\n",
             timestamp, settings.unitId[0] ? settings.unitId : "SEW", tag, levelStr, message);

    // 5. Vedno na Serial
    Serial.print(logLine);

    // 6. V RAM buffer (če inicializiran)
    if (loggingInitialized) {
        logBuffer += logLine;

        // Flush ko buffer preseže 10kB
        if (logBuffer.length() >= LOG_BUFFER_MAX) {
            flushBufferToSD();
        }
    }
}

// ============================================================
// Flush RAM bufferja na SD
// ============================================================
void flushBufferToSD() {
    static bool flushing = false;
    if (flushing) return;  // Prepreči rekurzijo
    flushing = true;

    if (!loggingInitialized || logBuffer.length() == 0) {
        flushing = false;
        return;
    }

    // Swap: premakni buffer v temp, počisti za nove loge
    String tempBuffer = logBuffer;
    logBuffer = "";

    // Ime datoteke po datumu
    String logFileName;
    if (timeSynced && myTZ.now() > 1577836800UL) {
        logFileName = "/log_" + myTZ.dateTime("Y-m-d") + ".txt";
    } else {
        logFileName = "/log_nodate.txt";
    }
    currentLogFile = logFileName;

    // Mutex za SD (deli SPI bus z LCD)
    if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        // Ne moremo dobiti mutexa - vrnemo buffer nazaj
        logBuffer = tempBuffer + logBuffer;
        if (logBuffer.length() > LOG_BUFFER_MAX * 3) {  // 36 kB cap (3× safety)
            Serial.printf("LOG: WARNING - Buffer overflow! Truncating %d → %d bytes\n",
                          logBuffer.length(), LOG_BUFFER_MAX);
            logBuffer = logBuffer.substring(logBuffer.length() - LOG_BUFFER_MAX);
            sensorData.err |= ERR_SD;  // Signal problema
        }
        flushing = false;
        return;
    }

    File f = SD.open(logFileName.c_str(), FILE_APPEND);
    if (!f) {
        if (sdMutex) xSemaphoreGive(sdMutex);
        // SD ni dostopen - vrnemo buffer nazaj da ne izgubimo logov
        logBuffer = tempBuffer + logBuffer;
        if (logBuffer.length() > LOG_BUFFER_MAX * 3) {  // 36 kB cap (3× safety)
            Serial.printf("LOG: WARNING - Buffer overflow! Truncating %d → %d bytes\n",
                          logBuffer.length(), LOG_BUFFER_MAX);
            logBuffer = logBuffer.substring(logBuffer.length() - LOG_BUFFER_MAX);
            sensorData.err |= ERR_SD;  // Signal problema
        }
        Serial.printf("LOG: SD flush failed - buffer restored (%d bytes)\n", logBuffer.length());
        flushing = false;
        return;
    }

    size_t written = f.print(tempBuffer);
    f.close();
    if (sdMutex) xSemaphoreGive(sdMutex);

    Serial.printf("LOG: Flushed %d bytes to %s\n", written, logFileName.c_str());
    flushing = false;
}

// ============================================================
// Cleanup starih log datotek (> 90 dni)
// ============================================================
void cleanupOldLogs() {
    if (!timeSynced || myTZ.now() <= 1577836800UL) {
        LOG_WARN("LOG", "cleanupOldLogs: NTP not synced - skipping cleanup");
        return;
    }

    // Izračunaj cutoff datum (90 dni nazaj) kot YYYYMMDD integer
    time_t cutoffTime = myTZ.now() - (90UL * 86400UL);
    struct tm cutoffTm;
    localtime_r(&cutoffTime, &cutoffTm);
    uint32_t cutoffDate = (uint32_t)(cutoffTm.tm_year + 1900) * 10000
                        + (uint32_t)(cutoffTm.tm_mon  + 1)    * 100
                        + (uint32_t) cutoffTm.tm_mday;

    LOG_INFO("LOG", "cleanupOldLogs: cutoff=%u (90 days ago)", cutoffDate);

    if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        LOG_WARN("LOG", "cleanupOldLogs: cannot acquire SD mutex");
        return;
    }

    File root = SD.open("/");
    if (!root) {
        if (sdMutex) xSemaphoreGive(sdMutex);
        LOG_ERROR("LOG", "cleanupOldLogs: cannot open root");
        return;
    }

    int deleted = 0;
    int checked = 0;

    // FIX: Iteracijski bug popravljen.
    // PREJ (napačno):
    //   while (entry) {
    //       String fileName = String(entry.name());
    //       entry.close();          <- zapre objekt
    //       ...SD.remove(...)...
    //       entry = root.openNextFile();  <- File objekt je bil neveljaven!
    //   }
    //
    // ZDAJ (pravilno):
    //   Shranimo ime datoteke v String, nato zapremo entry, nato
    //   naredimo remove() in pridobimo naslednji entry.
    //   SD.remove() kličemo ZUNAJ mutex/file context - mutex imamo
    //   že pridobljega zgoraj, zato direktno kličemo SD.remove().

    // Zberi seznam datotek za brisanje (ne briši med iteracijo!)
    // Razlog: nekatere SD implementacije ne podpirajo brisanja med odprtim root
    static String toDelete[100];
    int toDeleteCount = 0;

    File entry = root.openNextFile();
    while (entry && toDeleteCount < 100) {
        // FIX: Shrani ime PRED close()
        String fileName = String(entry.name());
        bool isDir = entry.isDirectory();
        entry.close();  // Varno zapremo - ime smo že shranili

        if (!isDir) {
            if (!fileName.startsWith("/")) fileName = "/" + fileName;

            // Format: /log_YYYY-MM-DD.txt  (19 znakov)
            if (fileName.startsWith("/log_") && fileName.endsWith(".txt") && fileName.length() == 19) {
                checked++;
                String datePart = fileName.substring(5, 15);  // "YYYY-MM-DD"
                datePart.replace("-", "");                      // "YYYYMMDD"
                uint32_t fileDate = datePart.toInt();

                if (fileDate > 0 && fileDate < cutoffDate) {
                    toDelete[toDeleteCount++] = fileName;
                }
            }
        }

        entry = root.openNextFile();
    }
    root.close();

    // Zdaj briši zbrane datoteke
    for (int i = 0; i < toDeleteCount; i++) {
        if (SD.remove(toDelete[i].c_str())) {
            LOG_INFO("LOG", "Deleted old log: %s", toDelete[i].c_str());
            deleted++;
        } else {
            LOG_ERROR("LOG", "Failed to delete: %s", toDelete[i].c_str());
        }
    }

    if (sdMutex) xSemaphoreGive(sdMutex);

    LOG_INFO("LOG", "cleanupOldLogs: checked=%d deleted=%d", checked, deleted);
}
