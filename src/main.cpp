// main.cpp - vent_SEW main entry point (graph_update.md 2026-03-04)
//
// ARHITEKTURA ZANKE (3 cone, §2 spec):
//
//   VSAK LOOP() PREHOD (~10ms):
//     runBsec()     — BSEC2 LP polling (neblokirajoča, kliče se pri vsakem prehodu)
//                     LP vzorči vsake ~3s → callback ob vsakem vzorcu, ostalo vrne false
//
//   HITRA ZANKA  (1s — FAST_TICK_INTERVAL):
//     readPIR()     — PIR edge detection + motionCount++ + startMotionRecording()
//     readBattery() — ADC baterija
//
//   GLAVNA ZANKA (3min — MAIN_CYCLE_INTERVAL):
//     readSHT41()       — SHT41: temp + hum
//     readTCS()         — TCS34725: lux + cct
//     updateWeather()   — OpenMeteo check (interno preverja 15-min interval)
//     graphStorePoint() — shrani GraphPoint v RAM + LittleFS
//     graphRefresh()    — posodobi LVGL graf
//     sendToREW()       — HTTP POST na REW
//     sensorData.motionCount = 0  — reset po pošiljanju
//     saveSDData()      — CSV log na SD
//     flushBufferToSD() — flush log bufferja
//
//   WIFI / NTP (10min — WIFI_CHECK_INTERVAL):
//     connectWifi()
//     syncNTP()
//
// SETUP vrstni red (12 korakov):
//   1. Serial
//   2. initGlobals() — NVS/settings/sensorData/timing
//   3. initLogging()
//   4. initSD()
//   5. WiFi + statični IP
//   6. NTP + myTZ
//   7. initDisplay()
//   8. initSens()
//   9. initCamera()
//  10. setupServer() (HTTP)
//  11. initWeather() + fetchWeatherNow()
//  12. graphStoreInit() + graphLoadFromLittleFS() + loadGraphPrefs()
//  13. checkAllDevices()
//
// SPREMEMBE (graph_update.md):
//   - GraphStorePoint → GraphPoint (nova struktura iz disp_graph.h)
//   - graphStoreAdd() → graphStorePoint()
//   - graphStoreLoad() → graphLoadFromLittleFS()
//   - gsCount → graphStoreCount()
//   - loadGraphPrefs() premaknjen: po initGraph() v setup()
//   - 3-conska zanka: hitra (1s), glavna (3min), WiFi (10min)
//   - lastFastTickMs kot globals timing var
//   - runBsec() klic pri VSAKEM loop() prehodu (ne v fast tick!)
//   - readPIR() + readBattery() v 1s hitro zanko
//   - readSHT41() + readTCS() v 3-min glavno zanko

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

        myTZ.setPosix(TZ_STRING);
        unsigned long myTZStart = millis();
        bool myTZReady = false;
        while (millis() - myTZStart < 15000) {
            events();
            if (myTZ.now() > 1577836800UL) { myTZReady = true; break; }
            delay(200);
        }

        if (myTZReady) {
            timeSynced = true;
            sensorData.err &= ~ERR_NTP;
            lastNtpSyncMs = millis();
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

    // 1. Globals: loadSettings() + sensorData init + timing reset
    initGlobals();

    // 2. Logging (MORA biti pred flushBufferToSD)
    initLogging();
    LOG_INFO("MAIN", "=== vent_SEW boot  ID:%s ===", settings.unitId);

    // 3. SD
    if (!initSD()) {
        LOG_WARN("MAIN", "SD init failed - continuing without SD");
    } else {
        sdPresent = true;
        LOG_INFO("MAIN", "SD OK");
    }

    // 4. WiFi - statični IP iz settings
    WiFi.mode(WIFI_STA);
    if (strlen(settings.localIP) > 0) {
        IPAddress ip, gw, sn(255, 255, 255, 0), dns(8, 8, 8, 8);
        if (ip.fromString(settings.localIP) && gw.fromString(settings.gateway)) {
            WiFi.config(ip, gw, sn, dns);
            LOG_INFO("MAIN", "Static IP: %s  GW: %s", settings.localIP, settings.gateway);
        } else {
            LOG_WARN("MAIN", "Invalid IP/GW - using DHCP");
        }
    }
    connectWifi();
    lastWifiCheckMs = millis();

    // 5. NTP + myTZ
    if (WiFi.status() == WL_CONNECTED) syncNTP();

    // 6. Zaslon
    initDisplay();
    lastTouchMs = millis();  // zaslon sveti prvih 10 minut po zagonu
    LOG_INFO("MAIN", "Display OK");

    // 7. Senzorji
    if (!initSens()) {
        LOG_WARN("MAIN", "SHT41 not found - continuing");
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
        LOG_INFO("MAIN", "HTTP server started");
    }

    // 10. Weather
    initWeather();
    if (WiFi.status() == WL_CONNECTED) fetchWeatherNow();

    // 11. Graf: LittleFS ring buffer + preference
    //   Vrstni red je obvezen:
    //   1) graphStoreInit()         — odpre LittleFS, ustvari/validira /graph.bin
    //   2) graphLoadFromLittleFS()  — RAM ← LittleFS (do 480 točk)
    //   3) loadGraphPrefs()         — NVS → currentGraphSensor, currentGraphHours
    //   initGraph() kliče disp.cpp → ne kliče loadGraphPrefs() sam!
    if (!graphStoreInit()) {
        LOG_WARN("MAIN", "graph_store init failed - graphs will be empty");
    } else {
        graphLoadFromLittleFS();
        LOG_INFO("MAIN", "graph_store OK: %d points loaded", graphStoreCount());
    }
    loadGraphPrefs();  // NVS → currentGraphSensor + currentGraphHours

    // 12. Startup validacija
    checkAllDevices();

    // Timing init (lastFastTickMs = 0 → prva iteracija sproži takoj)
    // initGlobals() že resteuje vse timing vars na 0
    // samo lastMainCycleMs postavimo nazaj, da se 3-min zanka ne sproži takoj
    // (senzorji potrebujejo čas za stabilizacijo)
    lastMainCycleMs = millis();  // prva glavna zanka čez 3 min

    LOG_INFO("MAIN", "=== Boot complete === ID:%s IP:%s ===",
             settings.unitId, WiFi.localIP().toString().c_str());

    // Initial log flush po boot-u (da vidimo startup loge na SD)
    if (sdPresent) flushBufferToSD();
}

// ============================================================
// LOOP — 3-conska arhitektura (graph_update.md §2)
// ============================================================
void loop() {
    unsigned long now = millis();

    // ------------------------------------------------------------------
    // PENDING WIFI RECONNECT — DEW pristop (identičen DEW main.cpp)
    // Nastavi ga: http.cpp POST /api/settings ob spremembi unitId
    // Logika:     Po 500ms zamiku (da HTTP odgovor prispe do brskalnika)
    //             nastavi novi statični IP in pokliče connectWifi().
    // 500ms je dovolj kratkek da brskalnik dobi odgovor, preden WiFi pade.
    // ------------------------------------------------------------------
    {
        static unsigned long pendingReconnectTime = 0;
        if (pendingWiFiReconnect) {
            if (pendingReconnectTime == 0) {
                pendingReconnectTime = now;
                LOG_INFO("MAIN", "WiFi reconnect scheduled — čakam 500ms za dostavo odgovora");
            } else if (now - pendingReconnectTime >= 500) {
                pendingWiFiReconnect = false;
                pendingReconnectTime = 0;
                LOG_INFO("MAIN", "WiFi reconnect z novim IP: %s gw: %s",
                         settings.localIP, settings.gateway);
                WiFi.disconnect(true);
                delay(200);
                IPAddress ip, gw, sn(255,255,255,0), dns(8,8,8,8);
                ip.fromString(settings.localIP);
                gw.fromString(settings.gateway);
                WiFi.config(ip, gw, sn, dns);
                connectWifi();
                lastWifiCheckMs = now;  // reset watchdog timer
                if (WiFi.status() == WL_CONNECTED) {
                    LOG_INFO("MAIN", "WiFi reconnected: %s", WiFi.localIP().toString().c_str());
                } else {
                    LOG_ERROR("MAIN", "WiFi reconnect failed po spremembi unitId");
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // CONA 1: WIFI / NTP watchdog — WIFI_CHECK_INTERVAL (10 min)
    // ------------------------------------------------------------------
    if (now - lastWifiCheckMs >= WIFI_CHECK_INTERVAL) {
        lastWifiCheckMs = now;
        connectWifi();
        if (WiFi.status() == WL_CONNECTED && !timeSynced) {
            syncNTP();
        }
        if (WiFi.status() == WL_CONNECTED && timeSynced &&
            now - lastNtpSyncMs >= NTP_UPDATE_INTERVAL) {
            syncNTP();
        }
    }

    // ------------------------------------------------------------------
    // CONA 2: HITRA ZANKA — FAST_TICK_INTERVAL (1 sekunda)
    //   - readPIR():     PIR edge detection, motionCount++
    //   - readBattery(): ADC baterija
    // ------------------------------------------------------------------
    if (now - lastFastTickMs >= FAST_TICK_INTERVAL) {
        lastFastTickMs = now;

        readPIR();      // PIR edge detection — ne blokira
        readBattery();  // ADC, ~16 vzorcev × 100µs ≈ 1.6ms

        // Periodični I2C health checks (interno preverja 10/30-min interval)
        performPeriodicSensorCheck();
        performPeriodicI2CReset();
    }

    // ------------------------------------------------------------------
    // CONA 3: GLAVNA ZANKA — MAIN_CYCLE_INTERVAL (3 minute)
    //   - readSHT41() + readTCS(): senzorji (blocking, ~50ms skupaj)
    //   - updateWeather(): OpenMeteo (interno preverja 15-min interval)
    //   - graphStorePoint(): shrani GraphPoint v RAM + LittleFS
    //   - graphRefresh(): posodobi LVGL graf
    //   - sendToREW(): HTTP POST
    //   - saveSDData(): CSV log
    //   - flushBufferToSD(): flush log bufferja
    // ------------------------------------------------------------------
    if (now - lastMainCycleMs >= MAIN_CYCLE_INTERVAL) {
        lastMainCycleMs = now;

        // Branje senzorjev (I2C)
        readSHT41();    // SHT41: temp + hum → sensorData.temp/hum
        readTCS();      // TCS34725: lux + cct → sensorData.lux/cct

        // Weather update (non-blocking, interno preverja 15-min interval)
        updateWeather();

        // Shrani GraphPoint v ring buffer (RAM + LittleFS)
        // Samo če imamo veljavno temperaturo (SHT41 mora biti prisoten)
        if (sensorData.temp > -900.0f) {
            GraphPoint pt;
            pt.ts    = (uint32_t)time(nullptr);
            pt.temp  = sensorData.temp;
            pt.hum   = sensorData.hum;
            pt.press = sensorData.press;    // BSEC2 (iz fast tick callback)
            pt.iaq   = (float)sensorData.iaq;
            pt.lux   = sensorData.lux;      // TCS34725
            pt.wind  = weatherData.valid ? weatherData.windSpeed      : 0.0f;
            pt.cloud = weatherData.valid ? (float)weatherData.cloudCover : 0.0f;
            graphStorePoint(pt);

            // Posodobi LVGL graf z novimi podatki
            graphRefresh();

            LOG_INFO("MAIN", "3min cycle: T=%.1f H=%.1f IAQ=%d lux=%.0f pts=%d",
                     sensorData.temp, sensorData.hum,
                     sensorData.iaq, sensorData.lux,
                     graphStoreCount());
        } else {
            LOG_WARN("MAIN", "3min cycle: skipping graphStore (SHT41 invalid)");
        }

        // HTTP POST na REW
        sendToREW();
        // Reset motionCount po pošiljanju (enotna točka reseta — ne v http.cpp!)
        sensorData.motionCount = 0;

        // CSV log na SD
        saveSDData();

        // Flush log RAM bufferja — ODSTRANJEN periodični flush!
        // Zdaj se logi zapisujejo SAMO ko buffer doseže 12 kB (LOG_BUFFER_MAX).
        // To zmanjša write operacije na SD z ~6× (prej vsake 3 min, zdaj ~20 min).
    }

    // ------------------------------------------------------------------
    // ENKRAT DNEVNO — brisanje starih video posnetkov
    // ------------------------------------------------------------------
    {
        static unsigned long lastCleanMs = 0;
        if (millis() - lastCleanMs >= 86400000UL) {  // 24h
            lastCleanMs = millis();
            cleanOldRecordings();
        }
    }

    // ------------------------------------------------------------------
    // STALNI PROCESI — pri vsakem loop() prehodu
    // ------------------------------------------------------------------

    // BSEC2: klic pri VSAKEM loop() prehodu (~10ms)
    // run() je neblokirajoča — vrne false takoj če ni čas za LP vzorec.
    // LP vzorči vsake ~3s → callback takrat, sicer brez I2C komunikacije.
    // Klic tukaj (ne v 1s fast tick!) zagotavlja, da BSEC dobi run()
    // tudi med dolgimi operacijami (HTTP POST, SD), ko fast tick ne pali.
    runBsec();

    // Kamera: podaljšanje snemanja ob gibanju
    performMotionRecordingCheck();

    // Stanje zaslona: vklop/izklop glede na PIR + touch + timeout
    updateScreenState();

    // LVGL / zaslon posodobitev (touch, animacije, draw)
    updateUI();

    delay(10);
}
