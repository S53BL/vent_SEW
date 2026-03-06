// weather.cpp - OpenMeteo weather data module for vent_SEW
//
// POPRAVKI (2026-03-02):
//   - FIX: DynamicJsonDocument(4096) → DynamicJsonDocument(8192)
//     OpenMeteo odgovor z forecast_days=1 in 11 hourly polji vsebuje 264 vrednosti
//     (~6-8 KB JSON). 4096 B NI DOVOLJ → NoMemory → heap korupcija → LVGL crash
//     (StoreProhibited pri lv_color_fill, EXCVADDR=0x00000000).
//   - FIX: Dodan StaticJsonDocument filter za zmanjšanje porabe RAM-a za ~60%.
//     Filter pove ArduinoJson naj alocira samo potrebna polja, ne celotnega dokumenta.
//   - FIX: payload velikost omejena na 12KB - zaščita pred prevelikim odgovorom.
//   - FIX: http.end() pred deserializeJson (sprosti TCP socket pomnilnik pred parsanjem).
//
#include "weather.h"
#include "icons.h"
#include "globals.h"
#include "logging.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
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

    // FIX: Omejitev velikosti odgovora (zaščita pred prevelikim odgovorom)
    int contentLen = http.getSize();
    if (contentLen > 12000) {
        LOG_ERROR("WEATHER", "Response too large: %d bytes", contentLen);
        http.end();
        lastFetchFailed = true;
        lastAttemptMs = millis();
        return false;
    }

    String payload = http.getString();
    // FIX: http.end() PRED parsanjem - sprosti TCP socket in heap pred alokacijo JSON doc
    http.end();

    LOG_INFO("WEATHER", "Payload size: %d bytes", payload.length());

    // FIX: DynamicJsonDocument(4096) → DynamicJsonDocument(8192)
    //
    // OpenMeteo odgovor z forecast_days=1 in 11 hourly polji:
    //   24 ur × 11 polj = 264 vrednosti + JSON overhead + hourly_units + metadata
    //   Skupaj: ~6000-8000 bajtov surovi JSON
    //
    // ArduinoJson v6 za parsanje potrebuje interno ~2× velikost podatkov.
    // 4096 B = PREMALO → deserializeJson vrne NoMemory.
    //
    // POSLEDICA NoMemory:
    //   doc je delno parsiran, vsi dostopi na doc["..."] vrnejo nullptr/0.
    //   Funkcija vrne false (OK), ampak weatherData.valid ostane false.
    //   Takoj sledi naslednji poskus (WEATHER_RETRY_INTERVAL = 60s), ki spet
    //   porabi heap. Heap fragmentacija povzroči LVGL alokacijo na NULL → crash.
    //
    // REŠITEV: 8192 B je dovolj za celoten OpenMeteo odgovor.
    //
    // Dodatna optimizacija: StaticJsonDocument filter - pove ArduinoJson
    // naj shrani SAMO potrebna polja (prihranek ~60% RAM-a pri parsanju).

    // Filter dokument - samo polja ki jih dejansko beremo
    StaticJsonDocument<256> filter;
    filter["current"]["weather_code"] = true;
    JsonObject fHourly = filter["hourly"].to<JsonObject>();
    fHourly["dew_point_2m"]            = true;
    fHourly["wind_speed_10m"]          = true;
    fHourly["precipitation"]           = true;
    fHourly["rain"]                    = true;
    fHourly["snowfall"]                = true;
    fHourly["cloud_cover"]             = true;
    fHourly["cloud_cover_low"]         = true;
    fHourly["cloud_cover_mid"]         = true;
    fHourly["cloud_cover_high"]        = true;
    fHourly["soil_temperature_0cm"]    = true;
    fHourly["soil_moisture_0_to_1cm"]  = true;

    // Glavni dokument - 8192 B (bil 4096 → NoMemory)
    DynamicJsonDocument doc(8192);
    DeserializationError err = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
    if (err) {
        LOG_ERROR("WEATHER", "JSON parse error: %s (payload=%d B, free heap=%u B)",
                  err.c_str(), payload.length(), ESP.getFreeHeap());
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

const lv_img_dsc_t* weatherCodeToImage(int code) {
    if (code == 0)                 return &SUN;
    if (code == 1)                 return &pSUN;
    if (code == 2)                 return &pCLD;
    if (code == 3)                 return &CLD;
    if (code == 45 || code == 48)  return &FOG;
    if (code >= 51 && code <= 55)  return &DZL;
    if (code >= 56 && code <= 57)  return &iDZL;
    if (code >= 61 && code <= 65)  return &RAIN;
    if (code >= 66 && code <= 67)  return &iRAIN;
    if (code >= 71 && code <= 75)  return &SNOW;
    if (code == 77)                return &SLEET;
    if (code >= 80 && code <= 82)  return &SHWR;
    if (code >= 85 && code <= 86)  return &SSHW;
    if (code == 95)                return &STRM;
    if (code >= 96 && code <= 99)  return &HAIL;
    return &NA;
}

// =============================================================================
// CLOUD UPLOADS - pomozne funkcije
// =============================================================================

static String _getCloudTimestamp() {
    if (!timeSynced) return "?";
    return myTZ.dateTime("Y-m-d H:i");
}

float calcDewPoint(float T, float RH) {
    return T - ((100.0f - RH) / 5.0f);
}

float calcSolarRad(float lux) {
    return lux * 0.0079f;
}

// =============================================================================
// WEATHERCLOUD UPLOAD
// =============================================================================

bool uploadToWeathercloud() {
    if (settings.wcIntervalMin == 0 ||
        strlen(settings.wcWid) == 0 ||
        strlen(settings.wcKey) == 0) {
        wcLastStatus = "disabled";
        return false;
    }

    if (WiFi.status() != WL_CONNECTED) {
        wcLastStatus = "ERR WiFi";
        LOG_WARN("WC", "Skipped - WiFi not connected");
        return false;
    }

    if (sensorData.temp  <= ERR_FLOAT + 1.0f ||
        sensorData.hum   <= ERR_FLOAT + 1.0f ||
        sensorData.press <= ERR_FLOAT + 1.0f) {
        wcLastStatus = "ERR no data";
        LOG_WARN("WC", "Skipped - sensor data invalid");
        return false;
    }

    float dew = calcDewPoint(sensorData.temp, sensorData.hum);
    float sr  = calcSolarRad(sensorData.lux > ERR_FLOAT + 1.0f ? sensorData.lux : 0.0f);

    char url[512];
    snprintf(url, sizeof(url),
        "http://api.weathercloud.net/v01/set"
        "?wid=%s&key=%s"
        "&temp=%d&hum=%d&bar=%d&dew=%d&solarrad=%d"
        "&ver=1.5&type=201",
        settings.wcWid,
        settings.wcKey,
        (int)(sensorData.temp  * 10.0f),
        (int)(sensorData.hum),
        (int)(sensorData.press * 10.0f),
        (int)(dew              * 10.0f),
        (int)(sr               * 10.0f)
    );

    LOG_DEBUG("WC", "URL: %s", url); // DEBUG - preveri pravilnost URL-ja pred pošiljanjem

    HTTPClient http;
    http.setTimeout(10000);
    http.setConnectTimeout(5000);
    http.begin(url);
    int httpCode = http.GET();
    http.end();

    if (httpCode == 200) {
        wcLastStatus = "OK " + _getCloudTimestamp();
        LOG_INFO("WC", "Upload OK: temp=%.1f hum=%d bar=%.1f dew=%.1f solarrad=%.1f",
                 sensorData.temp, (int)sensorData.hum,
                 sensorData.press, dew, sr);
        return true;
    } else {
        wcLastStatus = "ERR " + String(httpCode);
        LOG_WARN("WC", "Upload FAILED: HTTP %d", httpCode);
        return false;
    }
}

// =============================================================================
// WEATHER UNDERGROUND UPLOAD
// =============================================================================

bool uploadToWeatherUnderground() {
    if (settings.wuIntervalMin == 0 ||
        strlen(settings.wuStationID) == 0 ||
        strlen(settings.wuPassword)  == 0) {
        wuLastStatus = "disabled";
        return false;
    }

    if (WiFi.status() != WL_CONNECTED) {
        wuLastStatus = "ERR WiFi";
        LOG_WARN("WU", "Skipped - WiFi not connected");
        return false;
    }

    if (sensorData.temp  <= ERR_FLOAT + 1.0f ||
        sensorData.hum   <= ERR_FLOAT + 1.0f ||
        sensorData.press <= ERR_FLOAT + 1.0f) {
        wuLastStatus = "ERR no data";
        LOG_WARN("WU", "Skipped - sensor data invalid");
        return false;
    }

    float dew   = calcDewPoint(sensorData.temp, sensorData.hum);
    float sr    = calcSolarRad(sensorData.lux > ERR_FLOAT + 1.0f ? sensorData.lux : 0.0f);
    float tempF = sensorData.temp  * 1.8f + 32.0f;
    float dewF  = dew              * 1.8f + 32.0f;
    float baro  = sensorData.press * 0.02953f;

    char url[512];
    snprintf(url, sizeof(url),
        "https://rtupdate.wunderground.com/weatherstation/updateweatherstation.php"
        "?ID=%s&PASSWORD=%s"
        "&dateutc=now&action=updateraw"
        "&tempf=%.2f&humidity=%d&baromin=%.4f&dewptf=%.2f"
        "&solarradiation=%.1f&softwaretype=vent_SEW",
        settings.wuStationID,
        settings.wuPassword,
        tempF,
        (int)(sensorData.hum),
        baro,
        dewF,
        sr
    );

    // OBVEZNO WiFiClientSecure za HTTPS
    LOG_DEBUG("WU", "URL: %s", url); // DEBUG - preveri pravilnost URL-ja pred pošiljanjem

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setTimeout(10000);
    http.setConnectTimeout(5000);
    http.begin(client, url);  // NE http.begin(url) — ne deluje za https://

    int httpCode = http.GET();
    String payload = "";
    if (httpCode == 200) payload = http.getString();
    http.end();

    bool success = (httpCode == 200 && payload.indexOf("success") >= 0);
    if (success) {
        wuLastStatus = "OK " + _getCloudTimestamp();
        LOG_INFO("WU", "Upload OK: tempF=%.1f hum=%d baro=%.4f solarrad=%.1f",
                 tempF, (int)sensorData.hum, baro, sr);
    } else {
        wuLastStatus = "ERR " + String(httpCode);
        LOG_WARN("WU", "Upload FAILED: HTTP %d payload='%s'", httpCode, payload.c_str());
    }
    return success;
}
