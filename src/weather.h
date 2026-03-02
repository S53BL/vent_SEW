// weather.h - OpenMeteo weather data module for vent_SEW
#pragma once
#include <Arduino.h>

// ── OpenMeteo URL ──────────────────────────────────────────────────────────
// Ljubljana: 46.0569, 14.5058
#define METEO_URL \
  "https://api.open-meteo.com/v1/forecast" \
  "?latitude=46.0569&longitude=14.5058" \
  "&current=weather_code" \
  "&hourly=dew_point_2m,wind_speed_10m,precipitation,rain,snowfall" \
  ",cloud_cover,cloud_cover_low,cloud_cover_mid,cloud_cover_high" \
  ",soil_temperature_0cm,soil_moisture_0_to_1cm" \
  "&forecast_days=1" \
  "&timezone=Europe%2FLjubljana"

// ── Struct za vremenske podatke ────────────────────────────────────────────
struct WeatherData {
    // Current
    int     weatherCode;        // WMO koda vremena

    // Hourly (trenutna ura)
    float   dewPoint;           // Rosna točka [°C]
    float   windSpeed;          // Hitrost vetra 10m [km/h]
    float   precipitation;      // Padavine [mm]
    float   rain;               // Dež [mm]
    float   snowfall;           // Sneg [cm]
    int     cloudCover;         // Skupna pokritost oblakov [%]
    int     cloudCoverLow;      // Nizki oblaki [%]
    int     cloudCoverMid;      // Srednji oblaki [%]
    int     cloudCoverHigh;     // Visoki oblaki [%]
    float   soilTemp;           // Temperatura tal 0cm [°C]
    float   soilMoisture;       // Vlažnost tal 0-1cm [m³/m³]

    bool    valid;              // true = podatki so veljavni
    unsigned long lastFetch;    // millis() zadnjega uspešnega branja
};

extern WeatherData weatherData;

// Intervali
#define WEATHER_FETCH_INTERVAL  900000UL   // 15 minut
#define WEATHER_RETRY_INTERVAL   60000UL   // 1 minuta ob napaki

// ── Funkcije ───────────────────────────────────────────────────────────────
void initWeather();
void updateWeather();           // Klic iz main loop (preverja interval)
bool fetchWeatherNow();         // Takojšnje branje (vrne true = uspeh)

// Pomožne za UI
const char* weatherCodeToStr(int code);   // "Jasno", "Megleno" ...
const char* weatherCodeToIcon(int code);  // Simbol za zaslon
