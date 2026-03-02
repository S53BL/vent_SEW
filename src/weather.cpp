// weather.cpp - OpenMeteo weather data module for vent_SEW
#include "weather.h"
#include "globals.h"
#include "logging.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <time.h>

WeatherData weatherData = {};

static unsigned long lastAttemptMs = 0;
static bool lastFetchFailed = false;

void initWeather() {
    memset(&weatherData, 0, sizeof(weatherData));
    weatherData.valid = false;
    weatherData.weatherCode = -1;
    LOG_INFO("WEATHER", "Weather module initialized");
}

void updateWeather() {
    unsigned long now = millis();
    unsigned long interval = lastFetchFailed ? WEATHER_RETRY_INTERVAL : WEATHER_FETCH_INTERVAL;
    if (weatherData.lastFetch == 0 || (now - lastAttemptMs >= interval)) {
        fetchWeatherNow();
    }
}

bool fetchWeatherNow() {
    if (WiFi.status() != WL_CONNECTED) {
        LOG_WARN("WEATHER", "Skipped - WiFi not connected");
        lastFetchFailed = true;
        lastAttemptMs = millis();
        return false;
    }

    LOG_INFO("WEATHER", "Fetching weather from OpenMeteo...");
    HTTPClient http;
    http.setTimeout(10000);
    http.setConnectTimeout(5000);

    if (!http.begin(METEO_URL)) {
        LOG_ERROR("WEATHER", "http.begin failed");
        lastFetchFailed = true;
        lastAttemptMs = millis();
        return false;
    }

    int code = http.GET();
    if (code != 200) {
        LOG_ERROR("WEATHER", "HTTP error %d", code);
        http.end();
        lastFetchFailed = true;
        lastAttemptMs = millis();
        return false;
    }

    time_t now_t = time(nullptr);
    struct tm* ti = localtime(&now_t);
    int hourIdx = (ti->tm_hour < 24) ? ti->tm_hour : 0;

    String payload = http.getString();
    http.end();

    // FIX (2026-03-02): JsonDocument (v7) → DynamicJsonDocument (v6)
    // platformio.ini zahteva bblanchon/ArduinoJson @ ^6.21.0
    // V ArduinoJson v6 se uporablja DynamicJsonDocument(size) ali StaticJsonDocument<size>
    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        LOG_ERROR("WEATHER", "JSON parse error: %s", err.c_str());
        lastFetchFailed = true;
        lastAttemptMs = millis();
        return false;
    }

    weatherData.weatherCode   = doc["current"]["weather_code"] | -1;

    JsonObject hourly = doc["hourly"];
    weatherData.dewPoint      = hourly["dew_point_2m"][hourIdx]           | -999.0f;
    weatherData.windSpeed     = hourly["wind_speed_10m"][hourIdx]          | -999.0f;
    weatherData.precipitation = hourly["precipitation"][hourIdx]           | 0.0f;
    weatherData.rain          = hourly["rain"][hourIdx]                    | 0.0f;
    weatherData.snowfall      = hourly["snowfall"][hourIdx]                | 0.0f;
    weatherData.cloudCover    = hourly["cloud_cover"][hourIdx]             | 0;
    weatherData.cloudCoverLow = hourly["cloud_cover_low"][hourIdx]         | 0;
    weatherData.cloudCoverMid = hourly["cloud_cover_mid"][hourIdx]         | 0;
    weatherData.cloudCoverHigh= hourly["cloud_cover_high"][hourIdx]        | 0;
    weatherData.soilTemp      = hourly["soil_temperature_0cm"][hourIdx]    | -999.0f;
    weatherData.soilMoisture  = hourly["soil_moisture_0_to_1cm"][hourIdx]  | -999.0f;

    weatherData.valid = true;
    weatherData.lastFetch = millis();
    lastAttemptMs = millis();
    lastFetchFailed = false;

    LOG_INFO("WEATHER", "OK: wcode=%d dew=%.1f wind=%.1f precip=%.1f cloud=%d%% soil_t=%.1f soil_m=%.3f",
             weatherData.weatherCode, weatherData.dewPoint, weatherData.windSpeed,
             weatherData.precipitation, weatherData.cloudCover,
             weatherData.soilTemp, weatherData.soilMoisture);
    return true;
}

const char* weatherCodeToStr(int code) {
    if (code < 0)                  return "N/A";
    if (code == 0)                 return "Jasno";
    if (code == 1)                 return "Pretezno jasno";
    if (code == 2)                 return "Delno oblacno";
    if (code == 3)                 return "Oblacno";
    if (code == 45 || code == 48)  return "Megla";
    if (code >= 51 && code <= 55)  return "Rosenje";
    if (code >= 56 && code <= 57)  return "Ledeno rosenje";
    if (code >= 61 && code <= 65)  return "Dez";
    if (code >= 66 && code <= 67)  return "Ledeni dez";
    if (code >= 71 && code <= 75)  return "Sneg";
    if (code == 77)                return "Snezne kroglice";
    if (code >= 80 && code <= 82)  return "Plohe";
    if (code >= 85 && code <= 86)  return "Snezne plohe";
    if (code == 95)                return "Nevihta";
    if (code >= 96 && code <= 99)  return "Nevihta s toco";
    return "Neznano";
}

const char* weatherCodeToIcon(int code) {
    if (code < 0)                  return "?";
    if (code == 0)                 return "SUN";
    if (code == 1)                 return "pSUN";
    if (code == 2)                 return "pCLD";
    if (code == 3)                 return "CLD";
    if (code == 45 || code == 48)  return "FOG";
    if (code >= 51 && code <= 55)  return "DZL";
    if (code >= 56 && code <= 57)  return "iDZL";
    if (code >= 61 && code <= 65)  return "RAIN";
    if (code >= 66 && code <= 67)  return "iRAIN";
    if (code >= 71 && code <= 75)  return "SNOW";
    if (code == 77)                return "SLEET";
    if (code >= 80 && code <= 82)  return "SHWR";
    if (code >= 85 && code <= 86)  return "SSHW";
    if (code == 95)                return "STRM";
    if (code >= 96 && code <= 99)  return "HAIL";
    return "?";
}
