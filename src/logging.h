// logging.h - Logging system header for vent_SEW
// Identično DEW (kopirano iz REW):
//   - RAM buffer (flush pri 10kB)
//   - cleanupOldLogs - briše > 90 dni
//   - API (makroji, enum) - kompatibilen z REW/DEW

#ifndef LOGGING_H
#define LOGGING_H

#include <Arduino.h>
#include <stdarg.h>

// RAM buffer - flush ko preseže to mejo
#define LOG_BUFFER_MAX  10240   // 10kB

// Log level konstante
enum LogLevel {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO  = 1,
    LOG_LEVEL_WARN  = 2,
    LOG_LEVEL_ERROR = 3
};

// Funkcije
void initLogging();
void logEvent(LogLevel level, const char* tag, const char* format, ...);
void flushBufferToSD();
void cleanupOldLogs();

// Makroji - kompatibilni z REW/DEW
#define LOG_DEBUG(tag, format, ...) logEvent(LOG_LEVEL_DEBUG, tag, format, ##__VA_ARGS__)
#define LOG_INFO(tag, format, ...)  logEvent(LOG_LEVEL_INFO,  tag, format, ##__VA_ARGS__)
#define LOG_WARN(tag, format, ...)  logEvent(LOG_LEVEL_WARN,  tag, format, ##__VA_ARGS__)
#define LOG_ERROR(tag, format, ...) logEvent(LOG_LEVEL_ERROR, tag, format, ##__VA_ARGS__)

#endif // LOGGING_H
