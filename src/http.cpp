// http.cpp - HTTP communication module for vent_SEW
//
// Posiljanje podatkov na REW:
//   - sendToREW() se klice iz main loop vsakih settings.sendIntervalSec
//   - JSON vsebuje vse senzorske podatke + identiteto enote ("id" polje)
//   - Timeout: 10s pri prvem zagonu, 3s nato
//   - Connection retry: po napaki 30s pred naslednjim poskusom
//   - ERR_HTTP bitmask v sensorData.err ob napaki
//
// Web streznik (port 80):
//   GET  /              -> handleRoot() HTML status stran (via setupWebEndpoints)
//   GET  /status        -> JSON z aktualnimi vrednostmi senzorjev
//   GET  /api/ping      -> "pong"
//   GET  /api/settings  -> vrne aktualne settings kot JSON
//   POST /api/settings  -> posodobi Settings + shrani v NVS
//   POST /api/reset     -> reset settings na privzete vrednosti
//   GET  /update        -> OTA HTML stran
//   POST /update        -> OTA firmware flash
//   + kamera endpointi (setupCameraEndpoints): /cam/status, /cam/record/*
//   + web UI endpointi (setupWebEndpoints): /, /settings, /sd-list, /sd-file, /logs
//
// POPRAVKI (2026-03-01):
//   - setupServer() zdaj kliče setupCameraEndpoints() PRED server.begin()
//   - setupServer() zdaj kliče setupWebEndpoints() PRED server.begin()
//   - setupCameraEndpoints() deklarirana v http.h
//
// POPRAVKI (2026-03-01 v2):
//   - FIX: Odstranjen "GET /" redirect na "/status"
//     web.cpp setupWebEndpoints() registrira GET / → handleRoot() (HTML stran)
//     Dvojna registracija "/" bi povzročila nedefiniran vrstni red.
//   - FIX: Odstranjen sensorData.motionCount = 0 iz sendToREW()
//     Dvojni reset: sendToREW() in main.cpp loop sta oba resetirala motionCount.
//     main.cpp ga reseta vedno po izteku sendInterval (ne glede na HTTP rezultat).
//     Logika v sendToREW je bila: samo ob 200 OK → ampak main.cpp ga je resetiral
//     vseeno. Enotna točka reseta: samo main.cpp loop.
//
// POPRAVKI (2026-03-02):
//   - FIX: Dodan #include "cam.h" - zamenja extern deklaracije v lambdah.
//     extern CameraStatus getCameraStatus() znotraj lambda funkcije je bil
//     neveljaven C++ (nekateri prevajalniki ga zavrnejo, vedenje je undefined).
//     Pravilna rešitev: #include "cam.h" na vrhu → normalen klic funkcije.
//
// OTA:
//   - Identicna implementacija kot REW (ESPAsyncWebServer + Update.h)
//   - Po uspesnem flashu: 500ms delay + ESP.restart()

#include "http.h"
#include "cam.h"       // FIX: zamenja extern getCameraStatus/startMotionRecording/stopRecordingNow v lambdah
#include "web.h"
#include "globals.h"
#include "logging.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>

AsyncWebServer server(80);

// --- Connection state ---
static bool  firstHttpAttempt        = true;
static unsigned long lastConnFail    = 0;
#define CONN_RETRY_TIMEOUT_MS  30000UL   // 30s pred ponovnim poskusom
#define HTTP_TIMEOUT_FIRST_MS  10000     // 10s pri prvem zagonu
#define HTTP_TIMEOUT_MS         3000     // 3s normalno

// =============================================================================
// JSON PAYLOAD
// =============================================================================

String buildPayloadJSON() {
    StaticJsonDocument<512> doc;

    doc["id"]      = sensorData.unitId;

    // Temperatura in vlaznost (SHT41)
    if (sensorData.temp  > ERR_FLOAT + 1.0f) doc["temp"]  = serialized(String(sensorData.temp,  2));
    else                                       doc["temp"]  = nullptr;
    if (sensorData.hum   > ERR_FLOAT + 1.0f) doc["hum"]   = serialized(String(sensorData.hum,   1));
    else                                       doc["hum"]   = nullptr;

    // Tlak in BSEC (BME680)
    if (sensorData.press > ERR_FLOAT + 1.0f) doc["press"] = serialized(String(sensorData.press, 1));
    else                                       doc["press"] = nullptr;
    doc["iaq"]     = sensorData.iaq;
    doc["iaq_acc"] = sensorData.iaqAccuracy;
    doc["siaq"]    = sensorData.staticIaq;
    if (sensorData.eCO2      > ERR_FLOAT + 1.0f) doc["eco2"] = serialized(String(sensorData.eCO2,     1));
    else                                           doc["eco2"] = nullptr;
    if (sensorData.breathVOC > ERR_FLOAT + 1.0f) doc["bvoc"] = serialized(String(sensorData.breathVOC, 2));
    else                                           doc["bvoc"] = nullptr;

    // Svetloba (TCS34725)
    if (sensorData.lux > ERR_FLOAT + 1.0f) doc["lux"] = serialized(String(sensorData.lux, 1));
    else                                     doc["lux"] = nullptr;
    doc["cct"]  = sensorData.cct;
    doc["r"]    = sensorData.r;
    doc["g"]    = sensorData.g;
    doc["b"]    = sensorData.b;

    // Gibanje (PIR)
    doc["motion"] = sensorData.motion;
    doc["mcnt"]   = sensorData.motionCount;

    // Baterija
    if (sensorData.bat > ERR_FLOAT + 1.0f) doc["bat"] = serialized(String(sensorData.bat, 3));
    else                                     doc["bat"] = nullptr;
    doc["bat_pct"] = sensorData.batPct;

    // Napake
    doc["err"] = sensorData.err;

    // Unix timestamp (ce NTP synced)
    if (timeSynced) doc["ts"] = (unsigned long)myTZ.now();
    else            doc["ts"] = 0;

    String payload;
    serializeJson(doc, payload);
    return payload;
}

// =============================================================================
// SEND TO REW
// =============================================================================

int sendToREW() {
    // Preverimo WiFi
    if (WiFi.status() != WL_CONNECTED) {
        LOG_WARN("HTTP", "sendToREW: WiFi not connected");
        sensorData.err |= ERR_WIFI;
        sensorData.lastHttpCode = -2;
        return -2;
    }
    sensorData.err &= ~ERR_WIFI;

    // Connection backoff: ce je bila nedavna napaka, pocakamo
    if (!connection_ok && millis() - lastConnFail < CONN_RETRY_TIMEOUT_MS) {
        LOG_WARN("HTTP", "sendToREW: backoff (%lus remaining)",
                 (CONN_RETRY_TIMEOUT_MS - (millis() - lastConnFail)) / 1000);
        return -3;
    }

    String payload = buildPayloadJSON();
    LOG_DEBUG("HTTP", "Payload (%d B): %s", payload.length(), payload.c_str());

    HTTPClient http;
    int timeout = firstHttpAttempt ? HTTP_TIMEOUT_FIRST_MS : HTTP_TIMEOUT_MS;
    firstHttpAttempt = false;
    http.setTimeout(timeout);
    http.setConnectTimeout(timeout);

    String url = "http://" + String(settings.rewIP) + "/data";
    if (!http.begin(url)) {
        LOG_ERROR("HTTP", "begin() failed for %s", url.c_str());
        sensorData.err |= ERR_HTTP;
        sensorData.lastHttpCode = -1;
        lastConnFail = millis();
        connection_ok = false;
        http.end();
        return -1;
    }

    http.addHeader("Content-Type", "application/json");
    int httpCode = http.POST(payload);
    http.end();

    sensorData.lastHttpCode = httpCode;

    if (httpCode == HTTP_CODE_OK) {
        connection_ok = true;
        lastConnFail  = 0;
        retryAttempted = false;
        sensorData.err &= ~ERR_HTTP;
        sensorData.lastSendMs = millis();
        LOG_INFO("HTTP", "sendToREW OK -> %s (%d B, IAQ=%u acc=%d siaq=%u eco2=%.0f)",
                 url.c_str(), payload.length(),
                 sensorData.iaq, sensorData.iaqAccuracy,
                 sensorData.staticIaq, sensorData.eCO2);
    } else {
        connection_ok = false;
        lastConnFail  = millis();
        sensorData.err |= ERR_HTTP;
        LOG_WARN("HTTP", "sendToREW FAILED (HTTP %d) -> %s", httpCode, url.c_str());
    }

    return httpCode;
}

// =============================================================================
// REW PING
// =============================================================================

bool performREWPingCheck() {
    if (WiFi.status() != WL_CONNECTED) return false;

    HTTPClient http;
    int timeout = firstHttpAttempt ? HTTP_TIMEOUT_FIRST_MS : HTTP_TIMEOUT_MS;
    http.setTimeout(timeout);
    http.setConnectTimeout(timeout);

    String url = "http://" + String(settings.rewIP) + "/api/ping";
    if (!http.begin(url)) { http.end(); return false; }

    int code = http.GET();
    bool ok  = false;

    if (code == HTTP_CODE_OK) {
        String resp = http.getString();
        if (resp == "pong") {
            ok = true;
            connection_ok = true;
            sensorData.err &= ~ERR_HTTP;
            LOG_INFO("HTTP", "REW ping OK");
        } else {
            LOG_WARN("HTTP", "REW ping: unexpected response '%s'", resp.c_str());
        }
    } else {
        LOG_WARN("HTTP", "REW ping failed HTTP %d", code);
        connection_ok = false;
        sensorData.err |= ERR_HTTP;
        lastConnFail = millis();
    }

    http.end();
    return ok;
}

// =============================================================================
// OTA HTML
// =============================================================================

static const char OTA_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="sl"><head><meta charset="UTF-8">
<title>SEW OTA Update</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
*{box-sizing:border-box}
body{font-family:Arial,sans-serif;background:#101010;color:#e0e0e0;
     display:flex;flex-direction:column;align-items:center;padding:40px 20px}
h1{color:#4da6ff;margin-bottom:8px}
.sub{color:#888;font-size:14px;margin-bottom:30px}
.card{background:#1a1a1a;border:1px solid #333;border-radius:10px;
      padding:30px 36px;width:100%;max-width:480px;text-align:center}
input[type=file]{display:block;width:100%;padding:10px;margin:16px 0 20px;
  background:#2a2a2a;border:2px dashed #555;border-radius:6px;color:#e0e0e0;cursor:pointer}
input[type=file]:hover{border-color:#4da6ff}
.btn{display:inline-block;padding:12px 32px;background:#4da6ff;color:#101010;
     border:none;border-radius:6px;font-size:16px;font-weight:bold;cursor:pointer;width:100%}
.btn:hover{background:#6bb3ff}
.btn:disabled{background:#555;color:#888;cursor:not-allowed}
#progress{width:100%;background:#2a2a2a;border-radius:4px;height:18px;
           margin-top:18px;display:none;overflow:hidden}
#bar{height:100%;background:#4da6ff;width:0;transition:width 0.3s;border-radius:4px}
#status{margin-top:14px;font-size:14px;color:#4da6ff;min-height:20px}
.nav{margin-top:28px;font-size:14px}
.nav a{color:#4da6ff;text-decoration:none}
</style></head><body>
<h1>&#11014; OTA Firmware Update</h1>
<p class="sub" id="unit">SEW – Zunanja senzorska enota</p>
<div class="card">
  <form id="upForm">
    <input type="file" id="file" accept=".bin" required>
    <button class="btn" id="btn" type="submit">Nalozi firmware</button>
  </form>
  <div id="progress"><div id="bar"></div></div>
  <div id="status"></div>
</div>
<div class="nav"><a href="/">&#8592; Status</a></div>
<script>
document.getElementById('upForm').onsubmit=function(e){
  e.preventDefault();
  const f=document.getElementById('file').files[0];
  if(!f)return;
  const btn=document.getElementById('btn');
  const bar=document.getElementById('bar');
  const prog=document.getElementById('progress');
  const status=document.getElementById('status');
  btn.disabled=true;
  prog.style.display='block';
  status.textContent='Nalaganje...';
  const xhr=new XMLHttpRequest();
  xhr.upload.onprogress=function(e){
    if(e.lengthComputable){
      const pct=Math.round(e.loaded/e.total*100);
      bar.style.width=pct+'%';
      status.textContent='Nalaganje: '+pct+'%';
    }
  };
  xhr.onload=function(){
    if(xhr.status===200){
      bar.style.width='100%';
      bar.style.background='#44cc44';
      status.textContent='Uspelo! Naprava se resetira v 5s...';
      setTimeout(()=>{location.href='/';},5500);
    }else{
      bar.style.background='#ff4444';
      status.textContent='Napaka: '+xhr.responseText;
      btn.disabled=false;
    }
  };
  xhr.onerror=function(){status.textContent='Napaka pri prenosu!';btn.disabled=false;};
  const form=new FormData();
  form.append('update',f);
  xhr.open('POST','/update');
  xhr.send(form);
};
</script></body></html>
)rawliteral";

// =============================================================================
// CAMERA STATUS ENDPOINTS
// FIX (2026-03-02): extern deklaracije v lambdah ODSTRANJENE.
// Zamenjane z normalnimi klici - funkcije so deklaracije v cam.h (vključen zgoraj).
// =============================================================================

void setupCameraEndpoints() {
    // GET /cam/status -> JSON s stanjem kamere in zadnjim posnetkom
    server.on("/cam/status", HTTP_GET, [](AsyncWebServerRequest *request){
        // FIX: bil: extern CameraStatus getCameraStatus(); ← neveljaven C++ v lambdi
        CameraStatus cs = getCameraStatus();   // ← normalen klic; cam.h vključen zgoraj

        StaticJsonDocument<384> doc;
        doc["ready"]       = cs.ready;
        doc["streaming"]   = cs.streaming;
        doc["recording"]   = cs.recording;
        doc["rec_frames"]  = cs.recordFrames;
        doc["rec_secs"]    = cs.recordSecs;
        doc["stream_fps"]  = serialized(String(cs.streamFps, 1));
        doc["last_file"]   = cs.lastFile;
        doc["last_size"]   = cs.lastFileSize;
        doc["total_recs"]  = cs.totalRecordings;
        doc["stream_url"]  = "http://" + WiFi.localIP().toString() + ":81/stream";
        doc["capture_url"] = "http://" + WiFi.localIP().toString() + ":81/capture";

        String resp;
        serializeJsonPretty(doc, resp);
        request->send(200, "application/json", resp);
    });

    // POST /cam/record/start -> rocni zacetek snemanja (test / manual trigger)
    server.on("/cam/record/start", HTTP_POST, [](AsyncWebServerRequest *request){
        // FIX: bil: extern void startMotionRecording(); ← neveljaven C++ v lambdi
        startMotionRecording();   // ← normalen klic; cam.h vključen zgoraj
        request->send(200, "application/json", "{\"status\":\"recording_started\"}");
        LOG_INFO("HTTP", "/cam/record/start - manual trigger");
    });

    // POST /cam/record/stop -> rocna ustavitev snemanja
    server.on("/cam/record/stop", HTTP_POST, [](AsyncWebServerRequest *request){
        // FIX: bil: extern void stopRecordingNow(); ← neveljaven C++ v lambdi
        stopRecordingNow();   // ← normalen klic; cam.h vključen zgoraj
        request->send(200, "application/json", "{\"status\":\"recording_stopped\"}");
        LOG_INFO("HTTP", "/cam/record/stop");
    });

    LOG_INFO("HTTP", "Camera endpoints registered: /cam/status, /cam/record/start, /cam/record/stop");
}

// =============================================================================
// SETUP SERVER
// Vrstni red: setupCameraEndpoints() → setupWebEndpoints() → interni → server.begin()
// =============================================================================

bool setupServer() {
    LOG_INFO("HTTP", "Setting up server endpoints");

    // Registriraj kamera endpointe PRED server.begin()
    setupCameraEndpoints();

    // Registriraj web UI endpointe PRED server.begin()
    setupWebEndpoints();

    // --- GET /status -> JSON z aktualnimi vrednostmi ---
    server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request){
        StaticJsonDocument<768> doc;
        doc["id"]       = sensorData.unitId;
        doc["uptime"]   = millis() / 1000;
        doc["wifi_rssi"]= WiFi.RSSI();
        doc["ip"]       = WiFi.localIP().toString();
        doc["rew_ip"]   = settings.rewIP;

        // Senzorji
        if (sensorData.temp  > ERR_FLOAT + 1.0f) doc["temp"]  = sensorData.temp;
        if (sensorData.hum   > ERR_FLOAT + 1.0f) doc["hum"]   = sensorData.hum;
        if (sensorData.press > ERR_FLOAT + 1.0f) doc["press"] = sensorData.press;
        doc["iaq"]      = sensorData.iaq;
        doc["iaq_acc"]  = sensorData.iaqAccuracy;
        doc["siaq"]     = sensorData.staticIaq;
        if (sensorData.eCO2      > ERR_FLOAT + 1.0f) doc["eco2"] = sensorData.eCO2;
        if (sensorData.breathVOC > ERR_FLOAT + 1.0f) doc["bvoc"] = sensorData.breathVOC;
        if (sensorData.lux  > ERR_FLOAT + 1.0f) doc["lux"]  = sensorData.lux;
        doc["cct"]      = sensorData.cct;
        doc["motion"]   = sensorData.motion;
        doc["mcnt"]     = sensorData.motionCount;
        if (sensorData.bat  > ERR_FLOAT + 1.0f) doc["bat"]  = sensorData.bat;
        doc["bat_pct"]  = sensorData.batPct;
        doc["err"]      = sensorData.err;
        doc["http_ok"]  = (sensorData.lastHttpCode == 200);
        doc["last_http"]= sensorData.lastHttpCode;
        doc["conn_ok"]  = connection_ok;
        if (timeSynced) doc["ts"] = (unsigned long)myTZ.now();

        const char* accStr = "?";
        switch (sensorData.iaqAccuracy) {
            case 0: accStr = "unreliable"; break;
            case 1: accStr = "low";        break;
            case 2: accStr = "medium";     break;
            case 3: accStr = "calibrated"; break;
        }
        doc["iaq_acc_str"] = accStr;

        String resp;
        serializeJsonPretty(doc, resp);
        request->send(200, "application/json", resp);
    });

    // --- GET /api/ping -> pong ---
    server.on("/api/ping", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(200, "text/plain", "pong");
    });

    // --- POST /api/settings -> posodobi settings ---
    server.on("/api/settings", HTTP_POST,
        [](AsyncWebServerRequest *request){},
        NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len,
           size_t index, size_t total){
            static String body;
            if (index == 0) body = "";
            for (size_t i = 0; i < len; i++) body += (char)data[i];
            if (index + len != total) return;

            StaticJsonDocument<512> doc;
            DeserializationError err = deserializeJson(doc, body);
            if (err) {
                LOG_ERROR("HTTP", "/api/settings JSON error: %s", err.c_str());
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }

            bool changed = false;

            if (doc.containsKey("unitId")) {
                const char* v = doc["unitId"];
                if (v && strlen(v) > 0 && strlen(v) < 8) {
                    strncpy(settings.unitId, v, sizeof(settings.unitId) - 1);
                    settings.unitId[sizeof(settings.unitId) - 1] = '\0';
                    strncpy(sensorData.unitId, v, sizeof(sensorData.unitId) - 1);
                    sensorData.unitId[sizeof(sensorData.unitId) - 1] = '\0';
                    changed = true;
                }
            }
            if (doc.containsKey("rewIP")) {
                const char* v = doc["rewIP"];
                if (v && strlen(v) > 6) {
                    strncpy(settings.rewIP, v, sizeof(settings.rewIP) - 1);
                    settings.rewIP[sizeof(settings.rewIP) - 1] = '\0';
                    changed = true;
                }
            }
            if (doc.containsKey("tempOffset"))   { settings.tempOffset   = doc["tempOffset"];   changed = true; }
            if (doc.containsKey("humOffset"))     { settings.humOffset    = doc["humOffset"];    changed = true; }
            if (doc.containsKey("pressOffset"))   { settings.pressOffset  = doc["pressOffset"];  changed = true; }
            if (doc.containsKey("luxOffset"))     { settings.luxOffset    = doc["luxOffset"];    changed = true; }
            // FIX (2026-03-02): JSON ključi usklajeni z web_handlers.cpp JS:
            //   JS pošilja "sendInterval" (ne "sendIntervalSec")
            //   JS pošilja "readInterval" (ne "readIntervalSec")
            if (doc.containsKey("sendInterval"))  {
                uint16_t v = doc["sendInterval"];
                if (v >= 30 && v <= 3600) { settings.sendIntervalSec = v; changed = true; }
            }
            if (doc.containsKey("readInterval"))  {
                uint16_t v = doc["readInterval"];
                if (v >= 5 && v <= 600) { settings.readIntervalSec = v; changed = true; }
            }
            if (doc.containsKey("screenAlwaysOn"))   { settings.screenAlwaysOn   = doc["screenAlwaysOn"];   changed = true; }
            if (doc.containsKey("screenBrightness")) {
                uint16_t v = doc["screenBrightness"];
                if (v <= 1023) { settings.screenBrightness = v; changed = true; }
            }

            if (changed) {
                saveSettings();
                LOG_INFO("HTTP", "/api/settings updated: id=%s rew=%s sendInt=%d",
                         settings.unitId, settings.rewIP, settings.sendIntervalSec);
                request->send(200, "application/json", "{\"status\":\"OK\"}");
            } else {
                request->send(200, "application/json", "{\"status\":\"no_change\"}");
            }
        }
    );

    // --- GET /api/settings -> vrne aktualne nastavitve ---
    // FIX (2026-03-02): ključi usklajeni - vrača "sendInterval"/"readInterval"
    // kar JS v /settings strani prikaže v poljih #sendInterval / #readInterval
    server.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest *request){
        StaticJsonDocument<384> doc;
        doc["unitId"]          = settings.unitId;
        doc["localIP"]         = settings.localIP;
        doc["rewIP"]           = settings.rewIP;
        doc["tempOffset"]      = settings.tempOffset;
        doc["humOffset"]       = settings.humOffset;
        doc["pressOffset"]     = settings.pressOffset;
        doc["luxOffset"]       = settings.luxOffset;
        doc["sendInterval"]    = settings.sendIntervalSec;   // FIX: bilo sendIntervalSec
        doc["readInterval"]    = settings.readIntervalSec;   // FIX: bilo readIntervalSec
        doc["screenAlwaysOn"]  = settings.screenAlwaysOn;
        doc["screenBrightness"]= settings.screenBrightness;
        String resp;
        serializeJsonPretty(doc, resp);
        request->send(200, "application/json", resp);
    });

    // --- POST /api/reset -> reset settings na privzete ---
    server.on("/api/reset", HTTP_POST, [](AsyncWebServerRequest *request){
        resetSettings();
        LOG_WARN("HTTP", "/api/reset - settings reset to defaults");
        request->send(200, "application/json", "{\"status\":\"reset\"}");
    });

    // --- GET /update -> OTA HTML ---
    server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send_P(200, "text/html; charset=UTF-8", OTA_HTML);
    });

    // --- POST /update -> OTA flash ---
    server.on("/update", HTTP_POST,
        [](AsyncWebServerRequest *request){
            bool ok = !Update.hasError();
            String msg = ok ? "OK" : Update.errorString();
            AsyncWebServerResponse *resp = request->beginResponse(
                ok ? 200 : 500, "text/plain",
                ok ? "OK" : ("FAIL: " + msg));
            resp->addHeader("Connection", "close");
            request->send(resp);
            if (ok) { delay(500); ESP.restart(); }
        },
        [](AsyncWebServerRequest *request, String filename,
           size_t index, uint8_t *data, size_t len, bool final){
            if (!index) {
                LOG_INFO("OTA", "Start: %s (%u B)",
                         filename.c_str(), request->contentLength());
                if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH))
                    LOG_ERROR("OTA", "begin failed: %s", Update.errorString());
            }
            if (!Update.hasError() && Update.write(data, len) != len)
                LOG_ERROR("OTA", "write failed");
            if (final) {
                if (Update.end(true))
                    LOG_INFO("OTA", "OK: %u B", index + len);
                else
                    LOG_ERROR("OTA", "end failed: %s", Update.errorString());
            }
        }
    );

    LOG_INFO("HTTP", "Starting AsyncWebServer on port 80");
    server.begin();
    webServerRunning = true;
    LOG_INFO("HTTP", "Server running - unit=%s ip=%s rew=%s",
             settings.unitId, settings.localIP, settings.rewIP);
    return true;
}
