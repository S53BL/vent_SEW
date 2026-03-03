// main.cpp - vent_SEW main entry point
// Enota: Waveshare ESP32-S3-Touch-LCD-2
// Display: ST7789T3 240x320, LVGL
// WiFi -> POST /data na REW, GET OpenMeteo (15min)
//
// Setup koraki:
//   1. Serial
//   2. NVS / globals (loadSettings je znotraj initGlobals)
//   3. initLogging()        ← MORA biti pred prvim flushBufferToSD() klicem
//   4. SD (initSD)
//   5. WiFi (statični IP iz settings)
//   6. NTP + myTZ.setLocation()
//   7. Zaslon
//   8. Senzorji
//   9. Kamera
//  10. HTTP server
//  11. Weather (init + prvi fetch)
//  12. Startup validacija
//
// POPRAVKI (2026-02-28):
//   - initLogging() dodan v setup()
//   - myTZ.setLocation() dodan po NTP sinhronizaciji
//   - Odstranjen lokalni #define WIFI_CHECK_MS - duplikat WIFI_CHECK_INTERVAL
//
// POPRAVKI (2026-03-01):
//   - FIX: Lokalne static timing vars (lastWifiCheck, lastSensorRead, lastSensorSend)
//     zamenjane z globals (lastWifiCheckMs, lastSensorReadMs, lastSendMs iz globals.h)
//     Razlog: globals.cpp je definiral te spremenljivke (lastWifiCheckMs itd.)
//     ampak main.cpp jih ni nikoli bral ali pisal - namesto tega je imel
//     lastWifiCheck, lastSensorRead, lastSensorSend kot lokalne static.
//     Rezultat: globals timing vars so bile mrtev kod, nikoli posodobljene.
//     Sedaj: main.cpp bere in piše globals timing vars → checkAllDevices() in
//     morebitni drugi moduli imajo dostop do timing informacij.
//   - FIX: motionCount reset je samo tukaj (bil je duplikat v http.cpp sendToREW)
//     Enotna točka reseta: main.cpp loop po vsakem sendInterval → konsistentno

#include <Arduino.h>
#include "config.h"
#include "globals.h"
#include "logging.h"
#include "sd_card.h"
#include "sens.h"
#include "http.h"
#include "cam.h"
#include "disp.h"
#include "disp_graph.h"
#include "graph_store.h"
#include "weather.h"

#include <WiFi.h>
#include <time.h>
#include "wifi_config.h"

// FIX (2026-03-01): Lokalne static vars ODSTRANJENE - uporabljamo globals iz globals.h
// ODSTRANJENO: static unsigned long lastWifiCheck  = 0;  → lastWifiCheckMs  (globals)
// ODSTRANJENO: static unsigned long lastSensorRead = 0;  → lastSensorReadMs (globals)
// ODSTRANJENO: static unsigned long lastSensorSend = 0;  → lastSendMs       (globals)
// Razlog: globals.cpp definira lastWifiCheckMs, lastSensorReadMs, lastSendMs
//         ampak main.cpp jih ni pisal → mrtev kod v globals.
//         Sedaj main.cpp piše in bere direktno globals timing vars.

// ============================================================
// WiFi
// ============================================================
static void connectWifi() {
    if (WiFi.status() == WL_CONNECTED) return;
    LOG_INFO("MAIN", "Connecting WiFi...");
    WiFi.disconnect(true);
    delay(200);
    for (int i = 0; i < numNetworks; i++) {
        WiFi.begin(ssidList[i], passwordList[i]);
        unsigned long t0 = millis();
        while (millis() - t0 < 8000 && WiFi.status() != WL_CONNECTED) delay(200);
        if (WiFi.status() == WL_CONNECTED) {
            wifiSSID = String(ssidList[i]);
            sensorData.err &= ~ERR_WIFI;
            LOG_INFO("MAIN", "WiFi OK: %s  IP: %s  RSSI: %d dBm",
                     ssidList[i], WiFi.localIP().toString().c_str(), WiFi.RSSI());
            return;
        }
    }
    sensorData.err |= ERR_WIFI;
    LOG_WARN("MAIN", "WiFi connect failed");
}

// ============================================================
// NTP + ezTime timezone
// ============================================================
static void syncNTP() {
    setenv("TZ", TZ_STRING, 1);
    tzset();
    configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    time_t now = 0;
    unsigned long t0 = millis();
    while (time(&now) < 100000 && millis() - t0 < 10000) delay(200);
    if (now > 100000) {
        struct tm* ti = localtime(&now);
        char tbuf[32];
        strftime(tbuf, sizeof(tbuf), "%d.%m.%Y %H:%M:%S", ti);
        LOG_INFO("MAIN", "NTP sync OK: %s", tbuf);

        // Inicializiraj ezTime timezone po uspešni NTP sinhronizaciji
        myTZ.setPosix(TZ_STRING);
        LOG_INFO("MAIN", "myTZ set via POSIX: %s", TZ_STRING);

        // Čakaj da myTZ postane ready (events processing)
        unsigned long myTZStart = millis();
        bool myTZReady = false;
        while (millis() - myTZStart < 15000) {
            events();  // ezTime processing
            if (myTZ.now() > 1577836800UL) {  // 2020-01-01
                myTZReady = true;
                break;
            }
            delay(200);
        }

        if (myTZReady) {
            timeSynced = true;
            sensorData.err &= ~ERR_NTP;
            lastNtpSyncMs = millis();  // globals timing var
            LOG_INFO("MAIN", "myTZ ready: %s", myTZ.dateTime().c_str());
        } else {
            sensorData.err |= ERR_NTP;
            timeSynced = false;
            LOG_WARN("MAIN", "myTZ sync timeout (15s)");
        }
    } else {
        sensorData.err |= ERR_NTP;
        LOG_WARN("MAIN", "NTP sync timeout");
    }
}

// ============================================================
// SETUP
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n\n=== vent_SEW boot ===");

    // 1. globals: loadSettings() + sensorData init + sdMutex
    initGlobals();

    // 2. initLogging() MORA biti tukaj preden se kliče flushBufferToSD()
    initLogging();
    LOG_INFO("MAIN", "=== vent_SEW boot  ID:%s ===", settings.unitId);

    // 3. SD
    if (!initSD()) {
        LOG_WARN("MAIN", "SD init failed - continuing without SD");
    } else {
        sdPresent = true;
        LOG_INFO("MAIN", "SD OK");
    }

    // 4. WiFi - statični IP iz settings (string "192.168.2.191")
    WiFi.mode(WIFI_STA);
    if (strlen(settings.localIP) > 0) {
        IPAddress ip, gw, sn(255, 255, 255, 0), dns(8, 8, 8, 8);
        if (ip.fromString(settings.localIP) && gw.fromString(settings.gateway)) {
            WiFi.config(ip, gw, sn, dns);
            LOG_INFO("MAIN", "Static IP: %s  GW: %s", settings.localIP, settings.gateway);
        } else {
            LOG_WARN("MAIN", "Invalid IP/GW in settings - using DHCP");
        }
    }
    connectWifi();
    lastWifiCheckMs = millis();  // FIX: globals var

    // 5. NTP (+ myTZ.setLocation)
    if (WiFi.status() == WL_CONNECTED) syncNTP();

    // 6. Zaslon
    initDisplay();
    LOG_INFO("MAIN", "Display OK");

    // 7. Senzorji
    if (!initSens()) {
        LOG_WARN("MAIN", "SHT41 not found - continuing (other sensors may work)");
    }
    LOG_INFO("MAIN", "Sensors init done");

    // 8. Kamera
    if (!initCamera()) {
        LOG_WARN("MAIN", "Camera init failed");
    } else {
        LOG_INFO("MAIN", "Camera OK");
    }

    // 9. HTTP server
    if (!setupServer()) {
        LOG_WARN("MAIN", "HTTP server start failed");
    } else {
        LOG_INFO("MAIN", "HTTP server started at %s", WiFi.localIP().toString().c_str());
    }

    // 10. Weather
    initWeather();
    if (WiFi.status() == WL_CONNECTED) fetchWeatherNow();

    // 11. Graf: inicializacija LittleFS ring bufferja
    // graphStoreInit() ustvari/validira /graph.bin na LittleFS
    // graphStoreLoad() naloži obstoječo zgodovino v RAM → graf ni prazen po restartu
    if (!graphStoreInit()) {
        LOG_WARN("MAIN", "graph_store init failed - graphs will be empty");
    } else {
        graphStoreLoad();
        LOG_INFO("MAIN", "graph_store OK: %d points loaded", gsCount);
    }

    // 12. Startup validacija
    checkAllDevices();

    // Inicializiraj timing vars (globals)
    lastSensorReadMs = millis();
    lastSendMs       = millis();

    LOG_INFO("MAIN", "=== Boot complete === ID:%s IP:%s ===",
             settings.unitId, WiFi.localIP().toString().c_str());

    // Flush začetnih logov na SD
    if (sdPresent) flushBufferToSD();
}

// ============================================================
// LOOP
// ============================================================
void loop() {
    unsigned long now = millis();

    // WiFi watchdog - WIFI_CHECK_INTERVAL iz config.h (600000ms = 10min)
    // FIX: lastWifiCheckMs je globals var (prej: lokalna lastWifiCheck)
    if (now - lastWifiCheckMs >= WIFI_CHECK_INTERVAL) {
        lastWifiCheckMs = now;  // FIX: globals var
        connectWifi();
        if (WiFi.status() == WL_CONNECTED && !timeSynced) {
            syncNTP();
        }
        // Periodično posodabljanje NTP (vsakih 30 min, če je že sinhroniziran)
        if (WiFi.status() == WL_CONNECTED && timeSynced &&
            now - lastNtpSyncMs >= NTP_UPDATE_INTERVAL) {
            syncNTP();
        }
    }

    // Branje senzorjev po intervalu
    // FIX: lastSensorReadMs je globals var (prej: lokalna lastSensorRead)
    if (now - lastSensorReadMs >= (unsigned long)settings.readIntervalSec * 1000UL) {
        lastSensorReadMs = now;  // FIX: globals var
        runSens();
        readBattery();

        // Periodični checks
        performPeriodicSensorCheck();
        performPeriodicI2CReset();
    }

    // Pošiljanje na REW po intervalu
    // FIX: lastSendMs je globals var (prej: lokalna lastSensorSend)
    if (now - lastSendMs >= (unsigned long)settings.sendIntervalSec * 1000UL) {
        lastSendMs = now;  // FIX: globals var
        sendToREW();
        // FIX: motionCount reset je SAMO tukaj (bil duplikat v http.cpp sendToREW)
        // Reset vsakič po sendInterval, ne glede na HTTP rezultat
        sensorData.motionCount = 0;
        saveSDData();
    }

    // Weather update (non-blocking, interno preverja interval)
    updateWeather();

    // --- Glavna 3-minutna zanka ---
    // Vse operacije ki se dotikajo podatkov tečejo skupaj vsake 3 minute.
    // DATA_SEND_INTERVAL je definiran v config.h kot 180000UL (3 min).
    // newSensorData flag (iz sens.cpp) se ne briše tukaj — ostane za disp.cpp
    if (now - lastMainCycleMs >= DATA_SEND_INTERVAL) {
        lastMainCycleMs = now;

        // 1. Shrani točko v LittleFS ring buffer (graph_store)
        //    Samo če imamo vsaj veljavno temperaturo
        if (sensorData.temp > -900.0f) {
            GraphStorePoint pt;
            pt.ts     = (uint32_t)time(nullptr);
            pt.temp   = sensorData.temp;
            pt.hum    = sensorData.hum;
            pt.iaq    = (float)sensorData.iaq;
            pt.wind   = weatherData.valid ? weatherData.windSpeed : 0.0f;
            pt.motion = sensorData.motionCount;
            pt.pad    = 0;
            graphStoreAdd(pt);

            // 2. Osvezi LVGL graf na zaslonu
            graphRefresh();

            LOG_INFO("MAIN", "3min cycle: T=%.1f H=%.1f IAQ=%d pts=%d",
                     sensorData.temp, sensorData.hum, sensorData.iaq, gsCount);
        }

        // 3. Flush log RAM bufferja na SD (premaknjen sem iz 60s timera)
        if (sdPresent) flushBufferToSD();
    }

    // Kamera - podaljšanje snemanja ob gibanju
    performMotionRecordingCheck();

    // LVGL / zaslon update
    updateUI();

    // BSEC2 mora biti klican pogosto (vsaj vsake ~3s za LP mode)
    // run() je neblokirajoča - varno klicati pri vsakem loop() prehodu
    runBsecLoop();

    delay(10);
}
