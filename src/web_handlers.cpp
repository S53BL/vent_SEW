// web_handlers.cpp - HTTP handler implementacija za vent_SEW
//
// Strani:
//   GET /          → status (T, H, IAQ, kamera, baterija, WiFi, napake)
//   GET /settings  → nastavitve enote (unitId, IP, offseti, intervali)
//   GET /sd-list   → browser SD datotek (logi, posnetki AVI)
//   GET /sd-file   → download SD datoteke (?name=/recordings/abc.avi)
//   GET /logs      → RAM log buffer (zadnjih N vrstic)
//
// POPRAVKI (2026-02-28):
//   - handleSDList(): popravljen iteracijski bug v /recordings in root direktoriju.
//
// POPRAVKI (2026-03-02):
//   - FIX: JS save() funkcija v handleSettings() pošiljala napačne JSON ključe.
//     http.cpp POST /api/settings handler pričakuje: "sendInterval", "readInterval"
//     JS je pošiljal:                                "sendIntervalSec", "readIntervalSec"
//     → ključa se NIKOLI nista ujela → intervali se niso shranili v NVS.
//     Popravljene vrstici v save() objektu:
//       sendIntervalSec: parseInt(...)  →  sendInterval: parseInt(...)
//       readIntervalSec: parseInt(...)  →  readInterval: parseInt(...)

#include "web_handlers.h"
#include "globals.h"
#include "cam.h"
#include "logging.h"
#include "sd_card.h"
#include <WiFi.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <vector>
#include <algorithm>

// =============================================================================
// Skupni CSS + nav
// =============================================================================

static const char* CSS = R"(
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#111;color:#ddd;font-family:sans-serif;font-size:14px}
nav{background:#1a1a2e;padding:10px 20px;display:flex;gap:10px;flex-wrap:wrap}
nav a{color:#4da6ff;text-decoration:none;padding:5px 12px;
      border:1px solid #4da6ff;border-radius:5px;font-size:13px}
nav a:hover{background:#4da6ff;color:#111}
h1{color:#fff;margin:18px;font-size:19px}
h2{color:#4da6ff;font-size:14px;margin:14px 18px 5px 18px;
   border-bottom:1px solid #2a2a2a;padding-bottom:3px}
.wrap{max-width:860px;margin:0 auto 40px auto;padding:0 14px}
table{width:100%;border-collapse:collapse;margin-bottom:14px}
th,td{padding:7px 11px;text-align:left;border:1px solid #222;font-size:13px}
th{background:#1a1a1a;color:#999;width:38%}
td{background:#161616}
.ok{color:#4caf50;font-weight:bold}
.warn{color:#ffc107;font-weight:bold}
.err{color:#f44336;font-weight:bold}
.dim{color:#555}
.btn{display:inline-block;padding:7px 18px;border-radius:5px;cursor:pointer;
     font-size:13px;border:none;margin-top:6px;text-decoration:none}
.btn-blue{background:#1565c0;color:#fff}.btn-blue:hover{background:#1e88e5}
.btn-red{background:#b71c1c;color:#fff}.btn-red:hover{background:#e53935}
input,select{background:#1e1e1e;color:#ddd;border:1px solid #444;
             padding:5px 9px;border-radius:4px;font-size:13px;width:100%}
.log-box{background:#0a0a0a;border:1px solid #222;border-radius:4px;
         padding:10px;font-family:monospace;font-size:11px;
         max-height:60vh;overflow-y:auto;white-space:pre-wrap;word-break:break-all}
.log-INFO{color:#66bb6a}.log-WARN{color:#ffa726}
.log-ERROR{color:#ef5350}.log-DEBUG{color:#666}
.file-link{color:#4da6ff;text-decoration:none}
.file-link:hover{text-decoration:underline}
.tag-rec{background:#b71c1c;color:#fff;padding:1px 6px;
         border-radius:3px;font-size:11px;font-weight:bold}
.tag-log{background:#1565c0;color:#fff;padding:1px 6px;
         border-radius:3px;font-size:11px;font-weight:bold}
</style>)";

static String navBar() {
    return "<nav>"
           "<a href='/'>Status</a>"
           "<a href='/settings'>Nastavitve</a>"
           "<a href='/sd-list'>SD / Posnetki</a>"
           "<a href='/logs'>Logi</a>"
           "</nav>";
}

static String fmtBytes(size_t b) {
    if (b < 1024)       return String(b) + " B";
    if (b < 1024*1024)  return String(b/1024.0f, 1) + " KB";
    return                     String(b/(1024.0f*1024.0f), 1) + " MB";
}

static String iaqLabel(uint16_t iaq) {
    if (iaq <= 50)  return "<span class='ok'>Odlično (" + String(iaq) + ")</span>";
    if (iaq <= 100) return "<span class='ok'>Dobro (" + String(iaq) + ")</span>";
    if (iaq <= 150) return "<span class='warn'>Zmerno (" + String(iaq) + ")</span>";
    if (iaq <= 200) return "<span class='warn'>Slabo (" + String(iaq) + ")</span>";
    if (iaq <= 300) return "<span class='err'>Slabo (" + String(iaq) + ")</span>";
    return                 "<span class='err'>Nevarno (" + String(iaq) + ")</span>";
}

// =============================================================================
// GET / — status
// =============================================================================
void handleRoot(AsyncWebServerRequest* request) {
    LOG_INFO("WEB", "GET /");
    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
                  "<meta http-equiv='refresh' content='30'>"
                  "<title>SEW Status</title>";
    html += CSS;
    html += "</head><body>";
    html += navBar();
    html += "<h1>SEW — ";
    html += settings.unitId;
    html += " Status</h1><div class='wrap'>";

    // Senzorji
    html += "<h2>Okolje</h2><table>";
    char buf[40];
    snprintf(buf, sizeof(buf), "%.2f °C", sensorData.temp);
    html += "<tr><th>Temperatura</th><td>" + String(buf) + "</td></tr>";
    snprintf(buf, sizeof(buf), "%.1f %%", sensorData.hum);
    html += "<tr><th>Vlažnost</th><td>" + String(buf) + "</td></tr>";
    snprintf(buf, sizeof(buf), "%.1f hPa", sensorData.press);
    html += "<tr><th>Tlak</th><td>" + String(buf) + "</td></tr>";
    snprintf(buf, sizeof(buf), "%.1f lx  /  %u K", sensorData.lux, sensorData.cct);
    html += "<tr><th>Lux / CCT</th><td>" + String(buf) + "</td></tr>";
    html += "<tr><th>PIR gibanje</th><td>";
    html += sensorData.motion ? "<span class='warn'>DA</span>" : "ne";
    html += "  (skupaj: " + String(sensorData.motionCount) + ")</td></tr>";
    html += "</table>";

    // IAQ
    html += "<h2>Kakovost zraka (BSEC)</h2><table>";
    html += "<tr><th>IAQ</th><td>" + iaqLabel(sensorData.iaq) + "</td></tr>";
    html += "<tr><th>Static IAQ</th><td>" + iaqLabel(sensorData.staticIaq) + "</td></tr>";
    html += "<tr><th>Kalibracija</th><td>";
    const char* accNames[] = {"Nekalibrirano", "Nizka", "Srednja", "Kalibrirano"};
    uint8_t acc = min((uint8_t)3, sensorData.iaqAccuracy);
    html += (acc >= 3) ? "<span class='ok'>" : (acc >= 1 ? "<span class='warn'>" : "<span class='err'>");
    html += accNames[acc];
    html += " (";
    html += String(acc);
    html += "/3)</span></td></tr>";
    snprintf(buf, sizeof(buf), "%.0f ppm", sensorData.eCO2);
    html += "<tr><th>eCO2</th><td>";
    html += (sensorData.eCO2 > 1000) ? "<span class='warn'>" : "";
    html += buf;
    html += (sensorData.eCO2 > 1000) ? "</span>" : "";
    html += "</td></tr>";
    snprintf(buf, sizeof(buf), "%.2f ppm", sensorData.breathVOC);
    html += "<tr><th>breathVOC</th><td>" + String(buf) + "</td></tr>";
    html += "</table>";

    // Kamera
    CameraStatus cs = getCameraStatus();
    html += "<h2>Kamera</h2><table>";
    html += "<tr><th>Status</th><td>";
    html += cs.ready ? "<span class='ok'>Inicializirana</span>" : "<span class='err'>NAPAKA</span>";
    html += "</td></tr>";
    html += "<tr><th>Stream</th><td>";
    if (cs.streaming) {
        snprintf(buf, sizeof(buf), "AKTIVEN (%.1f fps)", cs.streamFps);
        html += "<span class='ok'>" + String(buf) + "</span>";
    } else {
        html += "<span class='dim'>ni klienta</span>";
    }
    html += "</td></tr>";
    html += "<tr><th>Snemanje</th><td>";
    if (cs.recording) {
        snprintf(buf, sizeof(buf), "AKTIVNO %us / %u fr", cs.recordSecs, cs.recordFrames);
        html += "<span class='err'>" + String(buf) + "</span>";
    } else {
        html += "<span class='dim'>mirovanje</span>";
    }
    html += "</td></tr>";
    html += "<tr><th>Stream URL</th><td><a class='file-link' href='http://";
    html += settings.localIP;
    html += ":81/stream' target='_blank'>:81/stream</a></td></tr>";
    html += "<tr><th>Snapshot</th><td><a class='file-link' href='http://";
    html += settings.localIP;
    html += ":81/capture' target='_blank'>:81/capture</a></td></tr>";
    snprintf(buf, sizeof(buf), "%u", cs.totalRecordings);
    html += "<tr><th>Skupaj posnetkov</th><td>" + String(buf) + "</td></tr>";
    if (cs.lastFile[0]) {
        html += "<tr><th>Zadnji posnetek</th><td><a class='file-link' href='/sd-file?name=/recordings/";
        html += cs.lastFile;
        html += "'>";
        html += cs.lastFile;
        html += "</a> (";
        html += fmtBytes(cs.lastFileSize);
        html += ")</td></tr>";
    }
    html += "</table>";

    // Sistem
    html += "<h2>Sistem</h2><table>";
    html += "<tr><th>Unit ID</th><td>" + String(settings.unitId) + "</td></tr>";
    html += "<tr><th>IP</th><td>" + WiFi.localIP().toString() + "</td></tr>";
    html += "<tr><th>REW IP</th><td>" + String(settings.rewIP) + "</td></tr>";
    html += "<tr><th>WiFi</th><td>";
    if (WiFi.status() == WL_CONNECTED) {
        snprintf(buf, sizeof(buf), "%s  %d dBm", wifiSSID.c_str(), WiFi.RSSI());
        html += "<span class='" + String(WiFi.RSSI() > -80 ? "ok" : "warn") + "'>" + buf + "</span>";
    } else {
        html += "<span class='err'>NI POVEZAVE</span>";
    }
    html += "</td></tr>";
    html += "<tr><th>Baterija</th><td>";
    snprintf(buf, sizeof(buf), "%.2f V / %u%%", sensorData.bat, sensorData.batPct);
    html += (sensorData.batPct < 30) ? "<span class='err'>" : (sensorData.batPct < 60 ? "<span class='warn'>" : "<span class='ok'>");
    html += buf;
    html += "</span></td></tr>";
    html += "<tr><th>Uptime</th><td>" + String(millis()/1000) + " s</td></tr>";
    snprintf(buf, sizeof(buf), "%u B", (unsigned)ESP.getFreeHeap());
    html += "<tr><th>Prosti heap</th><td>" + String(buf) + "</td></tr>";
    snprintf(buf, sizeof(buf), "0x%02X", sensorData.err);
    html += "<tr><th>Napake (bitmask)</th><td>";
    html += sensorData.err ? "<span class='err'>" : "<span class='ok'>";
    html += buf;
    html += "</span></td></tr>";
    html += "<tr><th>Zadnji HTTP</th><td>";
    html += (sensorData.lastHttpCode == 200) ? "<span class='ok'>200 OK</span>" :
            ("<span class='err'>" + String(sensorData.lastHttpCode) + "</span>");
    html += "</td></tr>";
    html += "</table>";

    html += "</div></body></html>";
    request->send(200, "text/html; charset=utf-8", html);
}

// =============================================================================
// GET /settings
// =============================================================================
void handleSettings(AsyncWebServerRequest* request) {
    LOG_INFO("WEB", "GET /settings");

    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
                  "<title>SEW Nastavitve</title>";
    html += CSS;
    // FIX (2026-03-02): JS save() objekt je pošiljal napačne ključe:
    //   sendIntervalSec → popravljeno v sendInterval
    //   readIntervalSec → popravljeno v readInterval
    // http.cpp POST /api/settings handler pričakuje "sendInterval" / "readInterval".
    // S starimi ključi se containsKey() ni nikoli ujemal → intervali se niso shranili.
    html += R"(
<script>
function save() {
    var d = {
        unitId:           document.getElementById('unitId').value,
        localIP:          document.getElementById('localIP').value,
        rewIP:            document.getElementById('rewIP').value,
        tempOffset:       parseFloat(document.getElementById('tempOffset').value),
        humOffset:        parseFloat(document.getElementById('humOffset').value),
        pressOffset:      parseFloat(document.getElementById('pressOffset').value),
        luxOffset:        parseFloat(document.getElementById('luxOffset').value),
        sendInterval:     parseInt(document.getElementById('sendInterval').value),
        readInterval:     parseInt(document.getElementById('readInterval').value),
        screenBrightness: parseInt(document.getElementById('brightness').value),
        screenAlwaysOn:   document.getElementById('screenAlwaysOn').checked
    };
    fetch('/api/settings',{method:'POST',
        headers:{'Content-Type':'application/json'},
        body:JSON.stringify(d)})
    .then(r=>r.json())
    .then(d=>{
        var m=document.getElementById('msg');
        m.className = d.status==='OK' ? 'ok' : 'err';
        m.textContent = d.status==='OK' ? 'Shranjeno!' : 'Napaka: '+(d.message||'?');
    }).catch(e=>{ document.getElementById('msg').textContent='Napaka: '+e; });
}
function resetDev() {
    if(confirm('Ponastaviti na privzete vrednosti?'))
        fetch('/api/reset',{method:'POST'})
        .then(r=>r.json())
        .then(()=>{ setTimeout(()=>location.reload(), 500); })
        .catch(e=>alert('Reset napaka: '+e));
}
</script>)";
    html += "</head><body>";
    html += navBar();
    html += "<h1>SEW — Nastavitve</h1><div class='wrap'>";

    html += "<h2>Identiteta</h2><table>";
    html += "<tr><th>Unit ID</th><td><input id='unitId' value='" + String(settings.unitId) + "' maxlength='7'></td></tr>";
    html += "<tr><th>Lokalni IP</th><td><input id='localIP' value='" + String(settings.localIP) + "'></td></tr>";
    html += "<tr><th>REW IP</th><td><input id='rewIP' value='" + String(settings.rewIP) + "'></td></tr>";
    html += "</table>";

    html += "<h2>Kalibracija</h2><table>";
    html += "<tr><th>Temp offset [°C]</th><td><input id='tempOffset' type='number' step='0.1' value='" + String(settings.tempOffset, 1) + "'></td></tr>";
    html += "<tr><th>Hum offset [%]</th><td><input id='humOffset' type='number' step='0.1' value='" + String(settings.humOffset, 1) + "'></td></tr>";
    html += "<tr><th>Press offset [hPa]</th><td><input id='pressOffset' type='number' step='0.1' value='" + String(settings.pressOffset, 1) + "'></td></tr>";
    html += "<tr><th>Lux offset [lx]</th><td><input id='luxOffset' type='number' step='0.5' value='" + String(settings.luxOffset, 1) + "'></td></tr>";
    html += "</table>";

    html += "<h2>Intervali</h2><table>";
    html += "<tr><th>Pošiljanje na REW [s]</th><td><input id='sendInterval' type='number' min='30' max='3600' value='" + String(settings.sendIntervalSec) + "'></td></tr>";
    html += "<tr><th>Branje senzorjev [s]</th><td><input id='readInterval' type='number' min='5' max='300' value='" + String(settings.readIntervalSec) + "'></td></tr>";
    html += "</table>";

    html += "<h2>Zaslon</h2><table>";
    html += "<tr><th>Svetlost (0-1023)</th><td><input id='brightness' type='number' min='0' max='1023' value='" + String(settings.screenBrightness) + "'></td></tr>";
    html += "<tr><th>Zaslon vedno vklopljen</th><td><input id='screenAlwaysOn' type='checkbox'" + String(settings.screenAlwaysOn ? " checked" : "") + "></td></tr>";
    html += "</table>";

    html += "<button class='btn btn-blue' onclick='save()'>Shrani</button>&nbsp;";
    html += "<button class='btn btn-red' onclick='resetDev()'>Ponastavi</button>";
    html += "<p id='msg' style='margin-top:10px; font-size:13px;'></p>";
    html += "</div></body></html>";
    request->send(200, "text/html; charset=utf-8", html);
}

// =============================================================================
// GET /sd-list
// =============================================================================
void handleSDList(AsyncWebServerRequest* request) {
    LOG_INFO("WEB", "GET /sd-list");
    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
                  "<title>SEW SD datoteke</title>";
    html += CSS;
    html += "</head><body>";
    html += navBar();
    html += "<h1>SEW — SD datoteke</h1><div class='wrap'>";

    if (!sdPresent) {
        html += "<p class='err'>SD kartica ni prisotna ali ni inicializirana.</p>";
        html += "</div></body></html>";
        request->send(503, "text/html; charset=utf-8", html);
        return;
    }

    // SD statistike
    html += "<h2>SD kartica</h2><table>";
    html += "<tr><th>Skupaj</th><td>" + fmtBytes(SD.totalBytes()) + "</td></tr>";
    html += "<tr><th>Zasedeno</th><td>" + fmtBytes(SD.usedBytes()) + "</td></tr>";
    html += "<tr><th>Prosto</th><td>" + fmtBytes(SD.totalBytes() - SD.usedBytes()) + "</td></tr>";
    html += "</table>";

    struct FInfo { String name; size_t size; };
    std::vector<FInfo> aviFiles, logFiles, otherFiles;

    if (SD.exists("/recordings")) {
        File dir = SD.open("/recordings");
        if (dir && dir.isDirectory()) {
            File f = dir.openNextFile();
            while (f) {
                String n      = String(f.name());
                size_t sz     = f.size();
                bool   isDir  = f.isDirectory();
                f.close();
                if (!isDir) {
                    if (n.endsWith(".avi") || n.endsWith(".AVI"))
                        aviFiles.push_back({"/recordings/" + n, sz});
                    else
                        otherFiles.push_back({"/recordings/" + n, sz});
                }
                f = dir.openNextFile();
            }
            dir.close();
        }
    }

    File root = SD.open("/");
    if (root && root.isDirectory()) {
        File f = root.openNextFile();
        while (f) {
            String n     = String(f.name());
            size_t sz    = f.size();
            bool   isDir = f.isDirectory();
            f.close();
            if (!isDir) {
                if (n.startsWith("log_") && n.endsWith(".txt"))
                    logFiles.push_back({"/" + n, sz});
                else if (!n.startsWith("recordings"))
                    otherFiles.push_back({"/" + n, sz});
            }
            f = root.openNextFile();
        }
        root.close();
    }

    auto sortDesc = [](const FInfo& a, const FInfo& b){ return a.name > b.name; };
    std::sort(aviFiles.begin(),   aviFiles.end(),   sortDesc);
    std::sort(logFiles.begin(),   logFiles.end(),   sortDesc);

    html += "<h2>Posnetki AVI (" + String(aviFiles.size()) + ")</h2>";
    if (aviFiles.empty()) {
        html += "<p class='dim' style='margin-bottom:12px'>Ni posnetkov.</p>";
    } else {
        html += "<table><tr><th>Datoteka</th><th>Velikost</th><th>Akcija</th></tr>";
        for (auto& f : aviFiles) {
            String fname = f.name.substring(f.name.lastIndexOf('/') + 1);
            html += "<tr><td><a class='file-link' href='/sd-file?name=" + f.name + "'>";
            html += fname + "</a></td><td>" + fmtBytes(f.size);
            html += "</td><td><a class='btn btn-blue' href='/sd-file?name=" + f.name + "' download>";
            html += "Download</a></td></tr>";
        }
        html += "</table>";
    }

    html += "<h2>Logi (" + String(logFiles.size()) + ")</h2>";
    if (logFiles.empty()) {
        html += "<p class='dim' style='margin-bottom:12px'>Ni logov.</p>";
    } else {
        html += "<table><tr><th>Datoteka</th><th>Velikost</th><th>Akcija</th></tr>";
        for (auto& f : logFiles) {
            String fname = f.name.substring(f.name.lastIndexOf('/') + 1);
            html += "<tr><td><span class='tag-log'>LOG</span> ";
            html += "<a class='file-link' href='/sd-file?name=" + f.name + "'>" + fname + "</a>";
            html += "</td><td>" + fmtBytes(f.size);
            html += "</td><td><a class='btn btn-blue' href='/sd-file?name=" + f.name + "' download>";
            html += "Download</a></td></tr>";
        }
        html += "</table>";
    }

    html += "</div></body></html>";
    request->send(200, "text/html; charset=utf-8", html);
}

// =============================================================================
// GET /sd-file?name=/recordings/2026-02-22_10-30-00.avi
// =============================================================================
void handleSDFile(AsyncWebServerRequest* request) {
    if (!request->hasParam("name")) {
        request->send(400, "text/plain", "Missing ?name=");
        return;
    }
    String path = request->getParam("name")->value();
    LOG_INFO("WEB", "GET /sd-file name=%s", path.c_str());

    if (!sdPresent || !SD.exists(path)) {
        request->send(404, "text/plain", "File not found: " + path);
        return;
    }

    String ct = "application/octet-stream";
    if (path.endsWith(".avi") || path.endsWith(".AVI")) ct = "video/x-msvideo";
    else if (path.endsWith(".txt"))  ct = "text/plain; charset=utf-8";
    else if (path.endsWith(".csv"))  ct = "text/csv; charset=utf-8";
    else if (path.endsWith(".jpg") || path.endsWith(".jpeg")) ct = "image/jpeg";

    request->send(SD, path, ct, true);
}

// =============================================================================
// GET /logs — RAM log buffer
// =============================================================================
void handleLogs(AsyncWebServerRequest* request) {
    LOG_INFO("WEB", "GET /logs");

    extern String logBuffer;   // iz logging.cpp (veljavna extern deklaracija v funkciji)

    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
                  "<title>SEW Logi</title>";
    html += CSS;
    html += R"(<script>
function autoRefresh() {
    var cb = document.getElementById('ar');
    if (cb && cb.checked) setTimeout(function(){ location.reload(); }, 10000);
}
window.onload = autoRefresh;
</script>)";
    html += "</head><body>";
    html += navBar();
    html += "<h1>SEW — RAM Logi</h1><div class='wrap'>";
    html += "<p style='color:#666;font-size:12px;margin-bottom:8px'>";
    html += "RAM buffer (zadnje meritve). ";
    html += "<label><input id='ar' type='checkbox' onchange='autoRefresh()'> Auto-refresh 10s</label>";
    html += "</p>";

    html += "<div class='log-box'>";
    if (logBuffer.isEmpty()) {
        html += "<span class='dim'>Log buffer je prazen.</span>";
    } else {
        int start = 0;
        while (start < (int)logBuffer.length()) {
            int end = logBuffer.indexOf('\n', start);
            if (end < 0) end = logBuffer.length();
            String line = logBuffer.substring(start, end);
            start = end + 1;

            String cssClass = "";
            if (line.indexOf(":INFO]")  >= 0) cssClass = "log-INFO";
            else if (line.indexOf(":WARN]")  >= 0) cssClass = "log-WARN";
            else if (line.indexOf(":ERROR]") >= 0) cssClass = "log-ERROR";
            else if (line.indexOf(":DEBUG]") >= 0) cssClass = "log-DEBUG";

            line.replace("&", "&amp;");
            line.replace("<", "&lt;");
            line.replace(">", "&gt;");

            if (cssClass.length() > 0)
                html += "<span class='" + cssClass + "'>" + line + "\n</span>";
            else
                html += line + "\n";
        }
    }
    html += "</div>";
    html += "<p style='color:#444;font-size:11px;margin-top:6px'>SD logi: <a class='file-link' href='/sd-list'>SD datoteke</a></p>";
    html += "</div></body></html>";
    request->send(200, "text/html; charset=utf-8", html);
}
