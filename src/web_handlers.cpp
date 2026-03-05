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

// Forward deklaracije
void processFileForDeletion(const String& fname, const String& todayStr, int& deletedCount, int& skippedCount);

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
.nav-home { background:#4caf50; color:#fff; border-radius:5px; padding:5px 12px;
            font-size:13px; text-decoration:none; line-height:1; display:inline-block; }
.nav-home:hover { background:#66bb6a; }
.nav-stream { background:#1565c0; color:#fff; border-radius:5px; padding:5px 12px;
              font-size:13px; text-decoration:none; line-height:1; display:inline-block; }
.nav-stream:hover { background:#1e88e5; }
</style>)";

static String navBar() {
    return "<nav>"
           "<a href='http://192.168.2.192/' class='nav-home' title='Domov'>&#127968;</a>"
           "<a href='/'>Status</a>"
           "<a href='/settings'>Nastavitve</a>"
           "<a href='/graphs'>Grafi</a>"
           "<a href='/sd-list'>SD / Posnetki</a>"
           "<a href='/logs'>Logi</a>"
           "<a href='/motion'>Gibanje</a>"
           "<a href='http://" + String(settings.localIP) + ":81/stream' class='nav-stream' title='Stream kamere' target='_blank'>&#128249;</a>"
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
        html += "<tr><th>Zadnji posnetek</th><td><a class='file-link' href='/sd-file?name=/video/";
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
    // IDENTITETA (2026-03-05):
    //   - localIP in gateway se NE pošiljata v JSON — sta computed iz unitId
    //   - JS vsebuje lookup tabelo SEW1..SEW5 → IP (enako kot sewIdToIP v config.h)
    //   - Po uspešnem shranjevanju prikaže sporočilo z novim IP naslovom
    //   - rewIP ni več nastavljiv prek web UI
    html += R"(
<script>
var SEW_IPS = {
    'SEW1': '192.168.2.195',
    'SEW2': '192.168.2.196',
    'SEW3': '192.168.2.197',
    'SEW4': '192.168.2.198',
    'SEW5': '192.168.2.199'
};
function sewIpForId(id) {
    return SEW_IPS[id] || '192.168.2.199';
}
function onUnitIdChange() {
    var id = document.getElementById('unitId').value;
    document.getElementById('computedIP').textContent = sewIpForId(id);
}
function save() {
    var unitId = document.getElementById('unitId').value;
    var d = {
        unitId:           unitId,
        tempOffset:       parseFloat(document.getElementById('tempOffset').value),
        humOffset:        parseFloat(document.getElementById('humOffset').value),
        pressOffset:      parseFloat(document.getElementById('pressOffset').value),
        luxOffset:        parseFloat(document.getElementById('luxOffset').value),
        sendInterval:     parseInt(document.getElementById('sendInterval').value),
        readInterval:     parseInt(document.getElementById('readInterval').value),
        screenBrightness: parseInt(document.getElementById('brightness').value),
        screenAlwaysOn:   document.getElementById('screenAlwaysOn').checked,
        videoKeepDays:    parseInt(document.getElementById('videoKeepDays').value)
    };
    fetch('/api/settings',{method:'POST',
        headers:{'Content-Type':'application/json'},
        body:JSON.stringify(d)})
    .then(r=>r.json())
    .then(function(resp){
        var m = document.getElementById('msg');
        if (resp.status === 'OK') {
            if (resp.newIP) {
                // Identiteta spremenjena — DEW pristop: čakaj 7s, nato redirect na novi IP
                m.className = 'ok';
                m.innerHTML = 'Shranjeno! WiFi se reconnecta na novi IP <b>' + resp.newIP + '</b>...';
                setTimeout(function(){
                    m.innerHTML = 'Redirecting na <b>' + resp.newIP + '</b>...';
                    setTimeout(function(){
                        window.location.href = 'http://' + resp.newIP + '/settings';
                    }, 1000);
                }, 7000);
            } else {
                m.className = 'ok';
                m.innerHTML = 'Shranjeno.';
            }
        } else {
            m.className = 'err';
            m.textContent = 'Napaka: ' + (resp.message || resp.status || '?');
        }
    }).catch(function(e){ document.getElementById('msg').textContent = 'Napaka: ' + e; });
}
function resetDev() {
    if(confirm('Ponastaviti na privzete vrednosti?'))
        fetch('/api/reset',{method:'POST'})
        .then(r=>r.json())
        .then(function(){ setTimeout(function(){ location.reload(); }, 500); })
        .catch(function(e){ alert('Reset napaka: '+e); });
}
</script>)";
    html += "</head><body>";
    html += navBar();
    html += "<h1>SEW — Nastavitve</h1><div class='wrap'>";

    // Identiteta: select SEW1..SEW5 z IP-ji, computed IP read-only prikaz
    // localIP in rewIP nista nastavljivi — LocalIP je computed, REW IP je fiksna konstanta
    html += "<h2>Identiteta</h2><table>";
    html += "<tr><th>Unit ID</th><td>";
    html += "<select id='unitId' onchange='onUnitIdChange()'>";
    const char* sewIds[] = {"SEW1","SEW2","SEW3","SEW4","SEW5"};
    const char* sewIps[] = {"192.168.2.195","192.168.2.196","192.168.2.197","192.168.2.198","192.168.2.199"};
    for (int i = 0; i < 5; i++) {
        html += "<option value='";
        html += sewIds[i];
        html += "'";
        if (strcmp(settings.unitId, sewIds[i]) == 0) html += " selected";
        html += ">";
        html += sewIds[i];
        html += " \xe2\x80\x94 ";
        html += sewIps[i];
        html += "</option>";
    }
    html += "</select></td></tr>";
    html += "<tr><th>IP naslov (computed)</th><td>";
    html += "<span id='computedIP' style='color:#4da6ff;font-weight:bold'>";
    html += settings.localIP;
    html += "</span>";
    html += " <span style='color:#555;font-size:11px'>(iz Unit ID — ni nastavljiv)</span>";
    html += "</td></tr>";
    html += "<tr><th>REW IP</th><td>";
    html += "<span style='color:#888'>";
    html += settings.rewIP;
    html += "</span>";
    html += " <span style='color:#555;font-size:11px'>(fiksna konstanta)</span>";
    html += "</td></tr>";
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

    html += "<h2>Video posnetki</h2><table>";
    html += "<tr><th>Hranjenje posnetkov [dni]</th><td><input id='videoKeepDays' type='number' min='1' max='30' value='" + String(settings.videoKeepDays) + "'></td></tr>";
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
        html += "<table><tr><th></th><th>Datoteka</th><th>Velikost</th><th>Akcija</th></tr>";
        for (auto& f : aviFiles) {
            String fname = f.name.substring(f.name.lastIndexOf('/') + 1);
            html += "<tr><td><input type='checkbox' class='del-cb' value='" + f.name + "'></td>";
            html += "<td><a class='file-link' href='/sd-file?name=" + f.name + "'>";
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
        html += "<table><tr><th></th><th>Datoteka</th><th>Velikost</th><th>Akcija</th></tr>";
        for (auto& f : logFiles) {
            String fname = f.name.substring(f.name.lastIndexOf('/') + 1);
            // Preveri, če je današnji log
            bool isToday = false;
            if (fname.startsWith("log_") && fname.endsWith(".txt")) {
                // Izvleci datum: med "log_" in ".txt"
                int dateStart = 4; // dolžina "log_"
                int dateEnd = fname.length() - 4; // dolžina ".txt"
                if (dateEnd > dateStart) {
                    String datePart = fname.substring(dateStart, dateEnd);
                    String todayStr = myTZ.dateTime("Ymd");
                    if (datePart == todayStr) {
                        isToday = true;
                    }
                }
            }
            html += "<tr><td>";
            if (isToday) {
                html += "<input type='checkbox' class='del-cb' value='" + f.name + "' disabled>";
            } else {
                html += "<input type='checkbox' class='del-cb' value='" + f.name + "'>";
            }
            html += "</td><td><span class='tag-log'>LOG</span> ";
            html += "<a class='file-link' href='/sd-file?name=" + f.name + "'>" + fname + "</a>";
            if (isToday) {
                html += " <span style='color:#ffc107;margin-left:6px'>DANES</span>";
            }
            html += "</td><td>" + fmtBytes(f.size);
            html += "</td><td><a class='btn btn-blue' href='/sd-file?name=" + f.name + "' download>";
            html += "Download</a></td></tr>";
        }
        html += "</table>";
    }

    html += R"(
    <button onclick='deleteSel()' class='btn btn-red' style='margin-top:10px'>
      Izbriši označene
    </button>

    <script>
    function deleteSel() {
        var cbs = document.querySelectorAll('.del-cb:checked');
        if (cbs.length === 0) { alert('Ni označenih datotek.'); return; }
        if (!confirm('Izbrisati ' + cbs.length + ' datotek?')) return;
        var names = Array.from(cbs).map(c => c.value).join(',');
        fetch('/api/delete-files', {
            method: 'POST',
            headers: {'Content-Type': 'application/x-www-form-urlencoded'},
            body: 'files=' + encodeURIComponent(names)
        })
        .then(r => r.json())
        .then(d => {
            alert(d.message || (d.success ? 'Izbrisano.' : 'Napaka.'));
            location.reload();
        })
        .catch(e => alert('Napaka: ' + e));
    }
    </script>
    )";

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
// Pomožna funkcija za obdelavo posamezne datoteke
void processFileForDeletion(const String& fname, const String& todayStr, int& deletedCount, int& skippedCount) {
    String sanitized = fname;
    
    // a. Trim whitespace
    sanitized.trim();
    if (sanitized.isEmpty()) return;
    
    // b. Sanitiziraj: odstrani ".." in "\\"
    if (sanitized.indexOf("..") >= 0 || sanitized.indexOf("\\\\") >= 0) {
        LOG_WARN("WEB", "Pot '%s' vsebuje nedovoljene znake (.. ali \\)", sanitized.c_str());
        return;
    }
    
    // c. Ime mora se začeti z "/" — če ne, dodaj "/"
    if (!sanitized.startsWith("/")) {
        sanitized = "/" + sanitized;
    }
    
    // d. Dovoljeni prefiksi poti: "/log_" ali "/recordings/"
    if (!sanitized.startsWith("/log_") && !sanitized.startsWith("/recordings/")) {
        LOG_WARN("WEB", "Nedovoljena datoteka: %s", sanitized.c_str());
        return;
    }
    
    // e. ZAŠČITA (samo za log datoteke, ne za AVI)
    if (sanitized.startsWith("/log_")) {
        // Izvleci datum: med "/log_" in ".txt"
        int dateStart = 5; // dolžina "/log_"
        int dateEnd = sanitized.indexOf(".txt");
        if (dateEnd > dateStart) {
            String datePart = sanitized.substring(dateStart, dateEnd);
            if (datePart == todayStr) {
                LOG_WARN("WEB", "Preskočen današnji log: %s", sanitized.c_str());
                skippedCount++;
                return;
            }
        }
    }
    
    // f. Brisanje datoteke
    if (SD.exists(sanitized)) {
        if (SD.remove(sanitized)) {
            LOG_INFO("WEB", "Izbrisana datoteka: %s", sanitized.c_str());
            deletedCount++;
        } else {
            LOG_WARN("WEB", "Brisanje datoteke ni uspelo: %s", sanitized.c_str());
        }
    } else {
        LOG_WARN("WEB", "Datoteka ne obstaja: %s", sanitized.c_str());
    }
}

// =============================================================================
// GET /api/graph-data?date=YYYY-MM-DD — stream CSV za grafe
// =============================================================================
void handleGraphData(AsyncWebServerRequest* request) {
    String date;
    if (request->hasParam("date")) {
        date = request->getParam("date")->value();
        if (date.length() != 10 || date.indexOf("..") >= 0) {
            request->send(400, "text/plain", "Invalid date");
            return;
        }
    } else {
        date = (timeSynced && myTZ.now() > 1577836800UL)
               ? myTZ.dateTime("Y-m-d") : "nodate";
    }
    String fn = "/sew_" + date + ".csv";
    LOG_INFO("WEB", "GET /api/graph-data date=%s", date.c_str());
    if (!SD.exists(fn.c_str())) {
        request->send(404, "text/plain", "No data: " + fn);
        return;
    }
    request->send(SD, fn.c_str(), "text/csv");
}

// =============================================================================
// GET /graphs — grafi temperature, vlage, pritiska, IAQ, Lux, gibanja
// =============================================================================
void handleGraphs(AsyncWebServerRequest* request) {
    LOG_INFO("WEB", "GET /graphs");
    String today = (timeSynced && myTZ.now() > 1577836800UL)
                   ? myTZ.dateTime("Y-m-d") : "";

    String html = "<!DOCTYPE html><html><head>"
                  "<meta charset='UTF-8'>"
                  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<title>SEW Grafi</title>";
    html += CSS;
    html += R"GCSS(<style>
.gc{display:flex;gap:6px;align-items:center;flex-wrap:wrap;
    padding:10px 16px;background:#161616;border-bottom:1px solid #1e1e1e;}
.gc label{color:#666;font-size:12px;}
.gb{background:#1a1a2e;color:#888;border:1px solid #2a2a2a;
    padding:4px 10px;border-radius:4px;font-size:12px;cursor:pointer;}
.gb.on,.gb:hover{background:#1565c0;color:#fff;border-color:#1565c0;}
.gblock{padding:10px 16px 0;}
.ghdr{color:#777;font-size:12px;margin-bottom:3px;
      display:flex;justify-content:space-between;align-items:baseline;}
.gval{color:#fff;font-size:14px;font-weight:bold;}
.gwrap{position:relative;height:200px;}
#gst{padding:5px 16px;font-size:11px;color:#555;background:#111;
     border-bottom:1px solid #181818;min-height:22px;}
@media (max-width: 600px) {
    .gwrap { height: 140px; }
    .gc    { gap: 8px; }
    .gb    { padding: 6px 12px; font-size: 13px; }
    .ghdr  { font-size: 13px; }
    .gval  { font-size: 13px; }
}
</style>)GCSS";

    html += "<script src='https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js'></script>";
    html += "<script src='https://cdn.jsdelivr.net/npm/chartjs-adapter-date-fns@3.0.0/dist/chartjs-adapter-date-fns.bundle.min.js'></script>";
    html += "<script src='https://cdn.jsdelivr.net/npm/hammerjs@2.0.8/hammer.min.js'></script>";
    html += "<script src='https://cdn.jsdelivr.net/npm/chartjs-plugin-zoom@2.0.1/dist/chartjs-plugin-zoom.min.js'></script>";
    html += "<script src='https://cdn.jsdelivr.net/npm/papaparse@5.4.1/papaparse.min.js'></script>";
    html += "</head><body>";
    html += navBar();
    html += "<h1>SEW \xe2\x80\x94 Grafi</h1>";

    // Kontrole — date input (default = danes)
    html += "<div class='gc'><label>Datum:</label>";
    html += "<input type='date' id='ds' value='" + today + "' onchange='loadD(this.value)'"
            " style='font-size:13px;padding:4px 8px;background:#222;color:#ddd;"
            "border:1px solid #444;border-radius:4px;'>";
    html += "<span style='color:#2a2a2a;'>|</span>";
    html += "<button class='gb' id='r1' onclick='setR(1,\"r1\")'>1h</button>";
    html += "<button class='gb' id='r6' onclick='setR(6,\"r6\")'>6h</button>";
    html += "<button class='gb' id='r24' onclick='setR(24,\"r24\")'>24h</button>";
    html += "<button class='gb on' id='rA' onclick='setR(0,\"rA\")'>Vse</button>";
    html += "<button class='gb' style='margin-left:auto;' onclick='rZ()'>&#8635; Reset zoom</button></div>";
    html += "<div id='gst'>Nalagam&#8230;</div>";

    // Canvas bloki — 6 grafov
    html += "<div class='gblock'><div class='ghdr'><span>&#127777; Temperatura</span>"
            "<span class='gval' id='vT'></span></div>"
            "<div class='gwrap'><canvas id='cT'></canvas></div></div>";
    html += "<div class='gblock' style='margin-top:6px;'><div class='ghdr'><span>&#128167; Vlaga</span>"
            "<span class='gval' id='vH'></span></div>"
            "<div class='gwrap'><canvas id='cH'></canvas></div></div>";
    html += "<div class='gblock' style='margin-top:6px;'><div class='ghdr'><span>&#128309; Pritisk</span>"
            "<span class='gval' id='vP'></span></div>"
            "<div class='gwrap'><canvas id='cP'></canvas></div></div>";
    html += "<div class='gblock' style='margin-top:6px;'><div class='ghdr'><span>&#128682; IAQ</span>"
            "<span class='gval' id='vI'></span></div>"
            "<div class='gwrap'><canvas id='cI'></canvas></div></div>";
    html += "<div class='gblock' style='margin-top:6px;'><div class='ghdr'><span>&#128161; Lux</span>"
            "<span class='gval' id='vL'></span></div>"
            "<div class='gwrap'><canvas id='cL'></canvas></div></div>";
    html += "<div class='gblock' style='margin-top:6px;'><div class='ghdr'><span>&#128694; Gibanje</span>"
            "<span class='gval' id='vMC'></span></div>"
            "<div class='gwrap'><canvas id='cM'></canvas></div></div>";

    html += R"GJS(<script>
var ch={},raw=[],rng=0,syn=false;
function oz(c){if(syn)return;syn=true;var mn=c.chart.scales.x.min,mx=c.chart.scales.x.max;['T','H','P','I','L','M'].forEach(function(k){if(ch[k]&&ch[k]!==c.chart)ch[k].zoomScale('x',{min:mn,max:mx},'none');});syn=false;}
var ZP={zoom:{wheel:{enabled:true},pinch:{enabled:true},mode:'x',onZoom:oz,onZoomComplete:oz},pan:{enabled:true,mode:'x',onPan:oz,onPanComplete:oz}};
function xsc(){return{type:'time',time:{tooltipFormat:'HH:mm:ss',displayFormats:{second:'HH:mm:ss',minute:'HH:mm',hour:'HH:mm',day:'d.M.'}},ticks:{color:'#555',maxTicksLimit:8},grid:{color:'#1e1e1e'}};}
function mkLine(id,col,unit,mn,mx){
  var ctx=document.getElementById(id);
  var ys={ticks:{color:'#555',callback:function(v){return v.toFixed(1)+unit;}},grid:{color:'#1e1e1e'}};
  if(mn!=null)ys.min=mn; if(mx!=null)ys.max=mx;
  return new Chart(ctx,{type:'line',data:{datasets:[{data:[],borderColor:col,backgroundColor:col+'18',borderWidth:1.5,pointRadius:0,fill:true,tension:0.2}]},options:{responsive:true,maintainAspectRatio:false,animation:false,interaction:{mode:'index',intersect:false},plugins:{legend:{display:false},tooltip:{backgroundColor:'#1e1e1e',borderColor:'#333',borderWidth:1,titleColor:'#aaa',bodyColor:'#fff',callbacks:{label:function(c){return c.parsed.y.toFixed(1)+unit;}}},zoom:ZP},scales:{x:xsc(),y:ys}}});
}
function iCh(){['T','H','P','I','L','M'].forEach(function(k){if(ch[k]){ch[k].destroy();}});ch={};ch.T=mkLine('cT','#ff9800','\u00b0C',null,null);ch.H=mkLine('cH','#4da6ff','%',0,100);ch.P=mkLine('cP','#ce93d8',' hPa',null,null);ch.I=mkLine('cI','#ef5350','',0,500);ch.L=mkLine('cL','#ffee58',' lx',0,null);ch.M=mkLine('cM','#66bb6a','',0,null);}
function rZ(){Object.keys(ch).forEach(function(k){if(ch[k])ch[k].resetZoom();});}
function setR(h,id){rng=h;['r1','r6','r24','rA'].forEach(function(i){var e=document.getElementById(i);if(e)e.className='gb'+(i===id?' on':'');});applyD(raw);}
function filt(d,h){if(!h||!d.length)return d;var mx=+d[d.length-1].timestamp;return d.filter(function(r){return +r.timestamp>=mx-h*3600;});}
function applyD(data){
  var d=filt(data,rng),sb=document.getElementById('gst');
  if(!d||!d.length){sb.textContent='Ni podatkov za izbrano obdobje.';return;}
  var tP=[],hP=[],pP=[],iP=[],lP=[],mP=[];
  d.forEach(function(r){var t=new Date(+r.timestamp*1000);tP.push({x:t,y:parseFloat(r.temp)||0});hP.push({x:t,y:parseFloat(r.hum)||0});pP.push({x:t,y:parseFloat(r.press)||0});iP.push({x:t,y:parseFloat(r.iaq)||0});lP.push({x:t,y:parseFloat(r.lux)||0});mP.push({x:t,y:parseFloat(r.motion_count)||0});});
  ch.T.data.datasets[0].data=tP;ch.H.data.datasets[0].data=hP;ch.P.data.datasets[0].data=pP;ch.I.data.datasets[0].data=iP;ch.L.data.datasets[0].data=lP;ch.M.data.datasets[0].data=mP;
  ch.T.update('none');ch.H.update('none');ch.P.update('none');ch.I.update('none');ch.L.update('none');ch.M.update('none');
  var l=d[d.length-1],f=d[0];
  document.getElementById('vT').textContent=parseFloat(l.temp).toFixed(1)+' \u00b0C';
  document.getElementById('vH').textContent=parseFloat(l.hum).toFixed(1)+' %';
  document.getElementById('vP').textContent=parseFloat(l.press).toFixed(1)+' hPa';
  document.getElementById('vI').textContent=parseFloat(l.iaq).toFixed(0);
  document.getElementById('vL').textContent=parseFloat(l.lux).toFixed(0)+' lx';
  document.getElementById('vMC').textContent=parseFloat(l.motion_count).toFixed(0);
  var dur=Math.round((+l.timestamp-+f.timestamp)/60);
  sb.textContent=d.length+' meritev | '+new Date(+f.timestamp*1000).toLocaleTimeString('sl-SI')+' \u2013 '+new Date(+l.timestamp*1000).toLocaleTimeString('sl-SI')+' ('+dur+' min)'+(d.length<data.length?' | skupaj: '+data.length:'');
  rZ();
}
function loadD(date){
  if(!date)return;
  var sb=document.getElementById('gst'); sb.textContent='Nalagam '+date+' \u2026';
  fetch('/api/graph-data?date='+encodeURIComponent(date))
  .then(function(r){if(!r.ok)throw new Error('HTTP '+r.status);return r.text();})
  .then(function(csv){Papa.parse(csv,{header:true,skipEmptyLines:true,complete:function(res){raw=res.data.filter(function(r){return r.timestamp&&r.timestamp!=='timestamp'&&!isNaN(+r.timestamp);});if(!raw.length){sb.textContent='Prazna datoteka.';return;}applyD(raw);},error:function(e){sb.textContent='CSV napaka: '+e.message;}});})
  .catch(function(e){sb.textContent='Napaka: '+e.message;});
}
window.addEventListener('load',function(){iCh();var d=document.getElementById('ds').value;if(d)loadD(d);else document.getElementById('gst').textContent='Ni razpoložljivega datuma - preverite NTP sinhronizacijo.';});
</script>)GJS";

    html += "</body></html>";
    request->send(200, "text/html; charset=utf-8", html);
}

// =============================================================================
// GET /logs — RAM log buffer
// =============================================================================
void handleLogs(AsyncWebServerRequest* request) {
    LOG_INFO("WEB", "GET /logs");

    extern String logBuffer;   // iz logging.cpp

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

// =============================================================================
// POST /api/delete-files — Brisanje označenih SD datotek
// =============================================================================
void handleDeleteFiles(AsyncWebServerRequest* request) {
    LOG_INFO("WEB", "POST /api/delete-files");

    // 1. Preveri parameter "files"
    if (!request->hasParam("files", true)) {
        String errJson = "{\"success\":false,\"error\":\"Manjka parameter files\"}";
        request->send(400, "application/json", errJson);
        return;
    }

    String filesParam = request->getParam("files", true)->value();
    if (filesParam.isEmpty()) {
        String errJson = "{\"success\":false,\"error\":\"Parameter files je prazen\"}";
        request->send(400, "application/json", errJson);
        return;
    }

    // 2. Preveri SD kartico
    if (!sdPresent) {
        String errJson = "{\"success\":false,\"error\":\"SD kartica ni na voljo\"}";
        request->send(500, "application/json", errJson);
        return;
    }

    // 3. Pridobi današnji datum
    String todayStr = myTZ.dateTime("Ymd");  // YYYYMMDD

    // 4. Inicializiraj števce
    int deletedCount = 0;
    int skippedCount = 0;

    // 5. Razčleni CSV seznam
    int start = 0;
    int end;
    while ((end = filesParam.indexOf(',', start)) >= 0) {
        String fname = filesParam.substring(start, end);
        fname.trim();
        start = end + 1;

        // Obdelaj datoteko
        processFileForDeletion(fname, todayStr, deletedCount, skippedCount);
    }
    // Zadnji element
    String lastFname = filesParam.substring(start);
    lastFname.trim();
    if (!lastFname.isEmpty()) {
        processFileForDeletion(lastFname, todayStr, deletedCount, skippedCount);
    }

    // 6. Sestavi JSON odgovor
    String responseJson;
    if (deletedCount == 0 && skippedCount == 0) {
        responseJson = "{\"success\":false,\"message\":\"Nobena datoteka ni bila izbrisana\"}";
    } else if (skippedCount == 0) {
        responseJson = "{\"success\":true,\"deleted\":" + String(deletedCount) + 
                      ",\"skipped\":0,\"message\":\"Izbrisano " + String(deletedCount) + " datotek\"}";
    } else {
        responseJson = "{\"success\":true,\"deleted\":" + String(deletedCount) + 
                      ",\"skipped\":" + String(skippedCount) + 
                      ",\"message\":\"Izbrisano " + String(deletedCount) + 
                      " datotek, " + String(skippedCount) + " današnjih preskočenih\"}";
    }

    request->send(200, "application/json", responseJson);
}

// =============================================================================
// GET /motion — gibanje: dnevni CSV log + seznam video posnetkov
// =============================================================================
void handleMotion(AsyncWebServerRequest* request) {
    LOG_INFO("WEB", "GET /motion");

    // Izberemo dan — URL param ?day=2026-03-04, privzeto danes
    String day;
    if (request->hasParam("day")) {
        day = request->getParam("day")->value();
    } else {
        day = timeSynced ? myTZ.dateTime("Y-m-d") : "";
    }

    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
                  "<title>SEW Gibanje</title>";
    html += CSS;
    html += "</head><body>";
    html += navBar();
    html += "<h1>SEW — Gibanje</h1><div class='wrap'>";

    // Trenutno stanje PIR
    html += "<h2>Trenutno</h2><table>";
    html += "<tr><th>PIR stanje</th><td>";
    html += sensorData.motion
        ? "<span class='err'>&#128994; AKTIVEN — gibanje zaznano</span>"
        : "<span class='ok'>&#9899; Mirovanje</span>";
    html += "</td></tr>";
    html += "<tr><th>Skupaj zaznav (od zagona)</th><td>" + String(sensorData.motionCount) + "</td></tr>";
    if (completedMotionTime > 0) {
        struct tm* ti = localtime((time_t*)&completedMotionTime);
        char tbuf[32];
        strftime(tbuf, sizeof(tbuf), "%d.%m.%Y %H:%M:%S", ti);
        html += "<tr><th>Zadnje gibanje</th><td>" + String(tbuf) + "</td></tr>";
    }
    html += "</table>";

    if (!sdPresent) {
        html += "<p class='warn'>SD kartica ni prisotna — ni arhiva gibanja.</p>";
        html += "</div></body></html>";
        request->send(200, "text/html; charset=utf-8", html);
        return;
    }

    // Izbira dne: dropdown iz obstoječih /motion/YYYY-MM-DD.csv
    std::vector<String> csvDays;
    if (SD.exists("/motion")) {
        File mdir = SD.open("/motion");
        if (mdir && mdir.isDirectory()) {
            File f = mdir.openNextFile();
            while (f) {
                String n = String(f.name());
                bool isDir = f.isDirectory();
                f.close();
                if (!isDir && n.endsWith(".csv") && n.length() == 14) {
                    // Format: YYYY-MM-DD.csv (10 chars + .csv = 14)
                    csvDays.push_back(n.substring(0, 10));
                }
                f = mdir.openNextFile();
            }
            mdir.close();
        }
    }
    std::sort(csvDays.begin(), csvDays.end(), [](const String& a, const String& b){ return a > b; });

    if (day.isEmpty() && !csvDays.empty()) day = csvDays[0];

    // Dropdown za izbiro dne
    html += "<h2>Dnevni arhiv</h2>";
    html += "<form method='get' style='margin-bottom:12px'>";
    html += "<select name='day' onchange='this.form.submit()' style='width:auto;min-width:160px'>";
    if (csvDays.empty()) {
        html += "<option>Ni podatkov</option>";
    }
    for (auto& d : csvDays) {
        html += "<option value='" + d + "'";
        if (d == day) html += " selected";
        html += ">" + d + "</option>";
    }
    html += "</select></form>";

    // Preberi CSV za izbrani dan
    if (!day.isEmpty()) {
        String csvPath = "/motion/" + day + ".csv";
        if (SD.exists(csvPath)) {
            // Preberi vsebino: pričakovane vrstice: timestamp,count ali timestamp
            // Povzetek: skupno zaznav, po uri
            int hourCounts[24] = {};
            int totalDay = 0;

            File f = SD.open(csvPath, FILE_READ);
            if (f) {
                while (f.available()) {
                    String line = f.readStringUntil('\n');
                    line.trim();
                    if (line.isEmpty() || line.startsWith("#")) continue;
                    // Format: UNIX_TS,count   ali samo UNIX_TS
                    int comma = line.indexOf(',');
                    time_t ts = 0;
                    int cnt = 1;
                    if (comma > 0) {
                        ts  = (time_t)line.substring(0, comma).toInt();
                        cnt = line.substring(comma + 1).toInt();
                    } else {
                        ts = (time_t)line.toInt();
                    }
                    if (ts > 0) {
                        struct tm* ti = localtime(&ts);
                        if (ti && ti->tm_hour >= 0 && ti->tm_hour < 24) {
                            hourCounts[ti->tm_hour] += cnt;
                            totalDay += cnt;
                        }
                    }
                }
                f.close();
            }

            html += "<h2>Povzetek: " + day + "  (skupaj: " + String(totalDay) + " zaznav)</h2>";

            // Urni histogram (preprosta bar tabela)
            html += "<table><tr><th style='width:60px'>Ura</th><th>Zaznave</th></tr>";
            int maxHour = 1;
            for (int h = 0; h < 24; h++) if (hourCounts[h] > maxHour) maxHour = hourCounts[h];
            for (int h = 0; h < 24; h++) {
                if (hourCounts[h] == 0) continue;
                int barW = (hourCounts[h] * 200) / maxHour;
                char tbuf[8];
                snprintf(tbuf, sizeof(tbuf), "%02d:00", h);
                html += "<tr><td>" + String(tbuf) + "</td><td>";
                html += "<div style='display:inline-block;background:#b71c1c;height:14px;width:"
                        + String(barW) + "px;vertical-align:middle'></div> ";
                html += "<span style='color:#ccc'>" + String(hourCounts[h]) + "</span>";
                html += "</td></tr>";
            }
            html += "</table>";
            html += "<p style='font-size:11px;color:#444;margin-top:4px'>"
                    "<a class='file-link' href='/sd-file?name=" + csvPath + "' download>CSV prenos</a></p>";
        } else {
            html += "<p class='dim'>Ni dnevnega CSV loga za " + day + ".</p>";
        }

        // AVI posnetki za ta dan: /video/YYYY-MM-DD/
        String videoDir = "/video/" + day;
        if (SD.exists(videoDir)) {
            std::vector<String> aviFiles;
            File vd = SD.open(videoDir);
            if (vd && vd.isDirectory()) {
                File f = vd.openNextFile();
                while (f) {
                    String n = String(f.name());
                    bool isDir = f.isDirectory();
                    f.close();
                    if (!isDir && (n.endsWith(".avi") || n.endsWith(".AVI")))
                        aviFiles.push_back(n);
                    f = vd.openNextFile();
                }
                vd.close();
            }
            std::sort(aviFiles.begin(), aviFiles.end());

            html += "<h2>Video posnetki: " + day + " (" + String(aviFiles.size()) + ")</h2>";
            if (aviFiles.empty()) {
                html += "<p class='dim'>Ni posnetkov za ta dan.</p>";
            } else {
                html += "<table><tr><th>Datoteka</th><th>Prenos</th></tr>";
                for (auto& n : aviFiles) {
                    String fullPath = videoDir + "/" + n;
                    // Velikost
                    File tmp = SD.open(fullPath);
                    size_t sz = tmp ? tmp.size() : 0;
                    if (tmp) tmp.close();
                    html += "<tr><td><a class='file-link' href='/sd-file?name=" + fullPath + "'>";
                    html += n + "</a>  <span class='dim'>" + fmtBytes(sz) + "</span></td>";
                    html += "<td><a class='btn btn-blue' href='/sd-file?name=" + fullPath + "' download>Download</a></td></tr>";
                }
                html += "</table>";
            }
        } else {
            html += "<p class='dim' style='margin-top:8px'>Ni video direktorija za " + day + ".</p>";
        }
    }

    // ==========================================================
    // SEKCIJA D — Skupna statistika (spec §5.2)
    // ==========================================================
    html += "<h2>Skupna statistika</h2><table>";

    // Skupaj zaznav danes (iz today's CSV)
    {
        String todayDay = timeSynced ? myTZ.dateTime("Y-m-d") : day;
        String todayCsv = "/motion/" + todayDay + ".csv";
        int todayTotal = 0;
        if (sdPresent && SD.exists(todayCsv)) {
            File f = SD.open(todayCsv, FILE_READ);
            if (f) {
                while (f.available()) {
                    String line = f.readStringUntil('\n');
                    line.trim();
                    if (line.isEmpty() || line.startsWith("#")) continue;
                    int comma = line.indexOf(',');
                    todayTotal += (comma > 0) ? line.substring(comma + 1).toInt() : 1;
                }
                f.close();
            }
        }
        html += "<tr><th>Skupaj zaznav danes</th><td>" + String(todayTotal) + "</td></tr>";
    }

    // Skupaj zaznav zadnjih 7 dni (seštevek iz vseh CSV datotek)
    {
        int week7total = 0;
        if (sdPresent && SD.exists("/motion")) {
            time_t now_t = time(nullptr);
            time_t cutoff7 = now_t - 7L * 86400L;
            File mdir = SD.open("/motion");
            if (mdir && mdir.isDirectory()) {
                File f = mdir.openNextFile();
                while (f) {
                    String n = String(f.name());
                    bool isDir = f.isDirectory();
                    f.close();
                    if (!isDir && n.endsWith(".csv") && n.length() == 14) {
                        // Parsiramo datum iz imena (YYYY-MM-DD.csv)
                        struct tm td = {};
                        String dstr = n.substring(0, 10);
                        if (strptime(dstr.c_str(), "%Y-%m-%d", &td)) {
                            time_t ft = mktime(&td);
                            if (ft >= cutoff7) {
                                String csvFull = "/motion/" + n;
                                File cf = SD.open(csvFull, FILE_READ);
                                if (cf) {
                                    while (cf.available()) {
                                        String line = cf.readStringUntil('\n');
                                        line.trim();
                                        if (line.isEmpty() || line.startsWith("#")) continue;
                                        int comma = line.indexOf(',');
                                        week7total += (comma > 0) ? line.substring(comma + 1).toInt() : 1;
                                    }
                                    cf.close();
                                }
                            }
                        }
                    }
                    f = mdir.openNextFile();
                }
                mdir.close();
            }
        }
        html += "<tr><th>Skupaj zaznav (7 dni)</th><td>" + String(week7total) + "</td></tr>";
    }

    // Prostor posnetkov na SD (/video/)
    if (sdPresent && SD.exists("/video")) {
        // Gremo čez vse dnevne direktorije in seštejemo velikosti
        size_t videoTotal = 0;
        File vroot = SD.open("/video");
        if (vroot && vroot.isDirectory()) {
            File dayD = vroot.openNextFile();
            while (dayD) {
                if (dayD.isDirectory()) {
                    String dayPath = "/video/" + String(dayD.name());
                    dayD.close();
                    File dd = SD.open(dayPath);
                    if (dd && dd.isDirectory()) {
                        File af = dd.openNextFile();
                        while (af) {
                            videoTotal += af.size();
                            af.close();
                            af = dd.openNextFile();
                        }
                    }
                    if (dd) dd.close();
                } else {
                    dayD.close();
                }
                dayD = vroot.openNextFile();
            }
            vroot.close();
        }
        html += "<tr><th>Prostor posnetkov (/video)</th><td>" + fmtBytes(videoTotal) + "</td></tr>";
    } else {
        html += "<tr><th>Prostor posnetkov (/video)</th><td class='dim'>n/a</td></tr>";
    }

    // Nastavljeno hranjenje
    html += "<tr><th>Hranjenje posnetkov</th><td>" + String(settings.videoKeepDays) + " dni"
            " <span class='dim'>(<a class='file-link' href='/settings'>sprememba</a>)</span></td></tr>";

    html += "</table>";

    html += "</div></body></html>";
    request->send(200, "text/html; charset=utf-8", html);
}
