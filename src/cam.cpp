// cam.cpp - Kamera modul za vent_SEW
//
// Inicializacija:    initCamera()      → esp_camera_init() + HTTP stream server
// Stream:            stream_task       → MJPEG multipart na port 81, /stream
// Snapshot:          capture_handler   → en JPEG frame na /capture
// Snemanje (AVI):    record_task       → sproži PIR, shrani na SD
//
// SPREMEMBE (2026-03-03):
//   - CAM_RECORD_FPS_TARGET: 7 → 3 fps
//     Posledica: ~2.3× manj podatkov, AVI_MAX_FRAMES zmanjšan na 400
//   - buildFilename(): nov format → /recordings/20260303_143000.avi
//     Alphabetski red = kronološki red → trivalno brisanje najstarejšega
//     Fallback (brez NTP): /recordings/NOSYNC_12345678.avi (millis/1000)
//   - cleanOldRecordings(): avtomatsko brisanje pred vsakim novim snemanjem
//     Kriterij: SD < CAM_SD_FREE_MIN_MB MB prostega prostora
//               ALI število datotek > CAM_MAX_RECORDINGS
//     Strategija: briši najstarejše (alphabetski = kronološki) dokler ni ok
//
// POPRAVKI (2026-02-28):
//   - Odstranjeni lokalni #define CAM_PIN_* (16 duplikatov config.h!)
//
// POPRAVKI (2026-03-01):
//   - KRITIČNO: _lastMotionMs zamenjana z globals lastMotionMs
//   - LEDC kanal in timer iz config.h
//   - OV5640_PID zaščiten z #ifndef
//
// POPRAVKI (2026-03-01 v2):
//   - KRITIČNO: AviWriter na HEAP (new/delete) — stack overflow fix
//   - Stack record_task: 8192 → 12288 B

#include "cam.h"
#include "avi.h"
#include "globals.h"
#include "logging.h"
#include "sd.h"
#include <esp_camera.h>
#include <esp_http_server.h>
#include <esp_timer.h>
#include <SD.h>
#include <Preferences.h>

// =============================================================================
// KONFIGURACIJA
// =============================================================================

#define CAM_STREAM_PORT          81
#define CAM_XCLK_FREQ_HZ         20000000
#define CAM_FRAME_SIZE           FRAMESIZE_SVGA    // 800×600
#define CAM_STREAM_QUALITY       12                // JPEG quality 0-63
#define CAM_RECORD_QUALITY       10

// SPREMEMBA (2026-03-03): 7 → 3 fps
// Pri 3fps in SVGA JPEG (~20-40KB/frame): ~1-2 MB/min, ~2-4 MB na 2-min posnetek
#define CAM_RECORD_FPS_TARGET     3
#define CAM_POST_MOTION_SEC      10
#define CAM_MAX_RECORD_SEC      120
#define CAM_RECORD_DIR           "/recordings"
#define CAM_RECORD_FRAME_MS      (1000 / CAM_RECORD_FPS_TARGET)  // 333ms med framei

// Brisanje posnetkov (2026-03-03)
// Preveri pred vsakim novim snemanjem - briši najstarejše dokler ni ok
#define CAM_MAX_RECORDINGS       50    // max število AVI datotek v /recordings/
#define CAM_SD_FREE_MIN_MB      200    // min MB prostega prostora na SD

// MJPEG stream
#define STREAM_BOUNDARY          "vent_sew_stream"
#define STREAM_CONTENT_TYPE      "multipart/x-mixed-replace;boundary=" STREAM_BOUNDARY
#define STREAM_BOUNDARY_LINE     "\r\n--" STREAM_BOUNDARY "\r\n"
#define STREAM_PART_HDR          "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n"

// =============================================================================
// INTERNE SPREMENLJIVKE
// =============================================================================

static bool              _camReady       = false;
static httpd_handle_t    _streamServer   = NULL;
static SemaphoreHandle_t _frameMutex     = NULL;
static TaskHandle_t      _recordTask     = NULL;

static volatile bool          _recordActive   = false;
static volatile unsigned long _recordStartMs  = 0;

static volatile uint32_t _totalRecordings = 0;
static volatile uint32_t _lastFileSize    = 0;
static char              _lastFilename[40] = "";

static volatile float    _streamFps     = 0.0f;
static volatile bool     _clientActive  = false;

// =============================================================================
// buildFilename()
//
// SPREMEMBA (2026-03-03): nov format s timestampom
//
// Format (ko je NTP sinhroniziran):
//   /recordings/20260303_143000.avi
//   Datoteke se urejajo alphabetsko = kronološko → enostavno brisanje najstarejše
//
// Format (brez NTP sinhronizacije):
//   /recordings/NOSYNC_12345678.avi
//   Uporabi millis()/1000 kot surrogat za čas — ni kronološko zanesljivo,
//   ampak zagotovi unikatno ime. NOSYNC_ prefix loči od sinhroniziranih datotek.
//
// Dolžina:  max 48 znakov (bufLen v record_task)
// =============================================================================

static void buildFilename(char* buf, size_t bufLen) {
    if (timeSynced) {
        // Format: YYYYmmdd_HHMMSS.avi
        // ezTime myTZ.dateTime() vrne string, ki ga formatiramo direktno
        snprintf(buf, bufLen, "%s/%s.avi",
                 CAM_RECORD_DIR,
                 myTZ.dateTime("Ymd_His").c_str());
    } else {
        // Brez NTP — millis() / 1000 kot unikatni identifikator
        snprintf(buf, bufLen, "%s/NOSYNC_%08lu.avi",
                 CAM_RECORD_DIR, millis() / 1000UL);
    }
}

static bool ensureRecordingDir() {
    if (!SD.exists(CAM_RECORD_DIR)) {
        if (!SD.mkdir(CAM_RECORD_DIR)) {
            LOG_ERROR("CAM", "Cannot create %s on SD", CAM_RECORD_DIR);
            return false;
        }
        LOG_INFO("CAM", "Created %s on SD", CAM_RECORD_DIR);
    }
    return true;
}

// =============================================================================
// cleanOldRecordings()
//
// NOVO (2026-03-03): briši najstarejše posnetke pred novim snemanjem
//
// Logika:
//   1. Preštej datoteke *.avi v CAM_RECORD_DIR
//   2. Izračunaj prosti prostor na SD
//   3. Če je preveč datotek ALI premalo prostora:
//      - Zberi seznam imen datotek v alphabetskem vrstnem redu
//        (alphabetski = kronološki za format YYYYmmdd_HHMMSS.avi)
//      - Briši od najstarejše naprej, dokler pogoji niso izpolnjeni
//
// Klic: znotraj record_task, pod sdMutex, PRED avi->open()
//
// Opomba o SD.totalBytes() / SD.usedBytes():
//   ESP32 Arduino SD knjižnica vrača te vrednosti v bajtih.
//   Za MB: delimo z (1024*1024).
//   SD.totalBytes() in SD.usedBytes() sta na voljo od esp32 Arduino >= 2.0.
// =============================================================================

static void cleanOldRecordings() {
    // Zberemo seznam AVI datotek
    File dir = SD.open(CAM_RECORD_DIR);
    if (!dir || !dir.isDirectory()) {
        LOG_WARN("CAM", "cleanOldRecordings: cannot open %s", CAM_RECORD_DIR);
        return;
    }

    // Zberemo imena v statičen array (max 100 datotek v buffer)
    // Heap alokacija ni varna v ISR kontekstu, tu smo v tasku → OK z new[]
    static const int MAX_LIST = 100;
    char** names = new char*[MAX_LIST];
    if (!names) {
        LOG_ERROR("CAM", "cleanOldRecordings: alloc failed");
        dir.close();
        return;
    }
    int count = 0;

    File f = dir.openNextFile();
    while (f && count < MAX_LIST) {
        const char* fname = f.name();
        // Upoštevamo samo .avi datoteke (ne mape, ne .log itd.)
        size_t flen = strlen(fname);
        if (!f.isDirectory() && flen > 4 &&
            strcasecmp(fname + flen - 4, ".avi") == 0)
        {
            names[count] = new char[flen + 1];
            if (names[count]) {
                strcpy(names[count], fname);
                count++;
            }
        }
        f.close();
        f = dir.openNextFile();
    }
    if (f) f.close();
    dir.close();

    if (count == 0) {
        delete[] names;
        return;
    }

    // Alphabetsko sortiranje (insertion sort — majhen N, enostaven)
    for (int i = 1; i < count; i++) {
        char* key = names[i];
        int j = i - 1;
        while (j >= 0 && strcmp(names[j], key) > 0) {
            names[j + 1] = names[j];
            j--;
        }
        names[j + 1] = key;
    }

    LOG_INFO("CAM", "cleanOldRecordings: %d AVI files found", count);

    // Preveri pogoje in briši od najstarejšega
    int deleted = 0;
    for (int i = 0; i < count; i++) {
        // Preveri pogoje (po vsakem brisanju)
        uint64_t freeBytes = SD.totalBytes() - SD.usedBytes();
        uint32_t freeMB    = (uint32_t)(freeBytes / (1024ULL * 1024ULL));
        int      remaining = count - deleted;

        bool tooMany  = (remaining > CAM_MAX_RECORDINGS);
        bool tooFull  = (freeMB < CAM_SD_FREE_MIN_MB);

        if (!tooMany && !tooFull) break;  // pogoji izpolnjeni

        // Sestavi polno pot
        char fullPath[80];
        snprintf(fullPath, sizeof(fullPath), "%s/%s", CAM_RECORD_DIR, names[i]);

        if (SD.remove(fullPath)) {
            LOG_INFO("CAM", "Deleted old recording: %s (free=%uMB, count=%d)",
                     names[i], freeMB, remaining);
            deleted++;
        } else {
            LOG_WARN("CAM", "Failed to delete: %s", fullPath);
        }
    }

    if (deleted > 0) {
        LOG_INFO("CAM", "cleanOldRecordings: deleted %d file(s)", deleted);
    }

    // Sprosti pomnilnik
    for (int i = 0; i < count; i++) {
        delete[] names[i];
    }
    delete[] names;
}

// =============================================================================
// RECORD TASK
// =============================================================================

static void record_task(void* arg) {
    LOG_INFO("CAM", "Record task started");

    AviWriter* avi = new AviWriter();
    if (!avi) {
        LOG_ERROR("CAM", "record_task: AviWriter heap alloc failed (OOM)!");
        _recordActive = false;
        vTaskDelete(NULL);
        return;
    }

    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        LOG_ERROR("CAM", "Cannot get sdMutex for recording start");
        delete avi;
        _recordActive = false;
        vTaskDelete(NULL);
        return;
    }

    ensureRecordingDir();

    // NOVO (2026-03-03): počisti stare posnetke PRED odprtjem nove datoteke
    cleanOldRecordings();

    char filename[48];
    buildFilename(filename, sizeof(filename));

    bool opened = avi->open(filename, 800, 600, CAM_RECORD_FPS_TARGET);
    xSemaphoreGive(sdMutex);

    if (!opened) {
        LOG_ERROR("CAM", "Failed to open AVI file: %s", filename);
        sensorData.err |= ERR_SD;
        delete avi;
        _recordActive = false;
        vTaskDelete(NULL);
        return;
    }

    strncpy(_lastFilename, filename + strlen(CAM_RECORD_DIR) + 1, sizeof(_lastFilename) - 1);
    _lastFilename[sizeof(_lastFilename) - 1] = '\0';

    sensorData.isRecording = true;
    sensorData.recordingFrames = 0;
    strncpy(sensorData.lastRecordingFile, _lastFilename, sizeof(sensorData.lastRecordingFile) - 1);
    sensorData.lastRecordingFile[sizeof(sensorData.lastRecordingFile) - 1] = '\0';

    LOG_INFO("CAM", "Recording: %s @ %dfps", filename, CAM_RECORD_FPS_TARGET);

    unsigned long frameMs     = CAM_RECORD_FRAME_MS;
    unsigned long lastFrameMs = millis();

    while (_recordActive) {
        unsigned long now     = millis();
        unsigned long elapsed = now - lastFrameMs;
        if (elapsed < frameMs) {
            vTaskDelay(pdMS_TO_TICKS(frameMs - elapsed));
        }
        lastFrameMs = millis();

        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) {
            LOG_WARN("CAM", "record: frame capture failed");
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        uint8_t* jpegBuf   = fb->buf;
        size_t   jpegLen   = fb->len;
        uint8_t* converted = NULL;

        if (fb->format != PIXFORMAT_JPEG) {
            size_t outLen = 0;
            if (!frame2jpg(fb, CAM_RECORD_QUALITY, &converted, &outLen)) {
                LOG_WARN("CAM", "record: JPEG conversion failed");
                esp_camera_fb_return(fb);
                continue;
            }
            jpegBuf = converted;
            jpegLen = outLen;
        }

        if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            avi->writeFrame(jpegBuf, jpegLen);
            xSemaphoreGive(sdMutex);
            sensorData.recordingFrames = avi->frameCount();
        } else {
            LOG_WARN("CAM", "record: SD mutex timeout - skipping frame");
        }

        if (converted) free(converted);
        esp_camera_fb_return(fb);
    }

    LOG_INFO("CAM", "Recording stopping: %u frames", avi->frameCount());

    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(3000)) == pdTRUE) {
        avi->close();
        xSemaphoreGive(sdMutex);
    } else {
        LOG_ERROR("CAM", "sdMutex timeout on AVI close!");
        avi->close();
    }

    _lastFileSize = avi->fileSize();
    _totalRecordings++;

    sensorData.isRecording     = false;
    sensorData.recordingFrames = 0;

    LOG_INFO("CAM", "Recording done: %s, %u frames, %u B, total=%u",
             filename, avi->frameCount(), _lastFileSize, _totalRecordings);

    delete avi;
    avi = NULL;

    _recordTask = NULL;
    vTaskDelete(NULL);
}

// =============================================================================
// STREAM HANDLER
// =============================================================================

static esp_err_t stream_handler(httpd_req_t* req) {
    LOG_INFO("CAM", "Stream client connected");
    _clientActive = true;

    esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res != ESP_OK) { _clientActive = false; return res; }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");

    char partHdr[64];
    int64_t lastFrame = esp_timer_get_time();
    uint32_t frameCount = 0;
    float fps = 0;

    while (true) {
        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) {
            LOG_WARN("CAM", "stream: frame capture failed");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        uint8_t* jpegBuf   = fb->buf;
        size_t   jpegLen   = fb->len;
        uint8_t* converted = NULL;

        if (fb->format != PIXFORMAT_JPEG) {
            size_t outLen = 0;
            if (!frame2jpg(fb, CAM_STREAM_QUALITY, &converted, &outLen)) {
                esp_camera_fb_return(fb);
                continue;
            }
            jpegBuf = converted;
            jpegLen = outLen;
        }

        res = httpd_resp_send_chunk(req, STREAM_BOUNDARY_LINE, strlen(STREAM_BOUNDARY_LINE));
        if (res == ESP_OK) {
            int hLen = snprintf(partHdr, sizeof(partHdr), STREAM_PART_HDR, (unsigned)jpegLen);
            res = httpd_resp_send_chunk(req, partHdr, hLen);
        }
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, (const char*)jpegBuf, jpegLen);
        }

        if (converted) free(converted);
        esp_camera_fb_return(fb);

        if (res != ESP_OK) break;

        int64_t now = esp_timer_get_time();
        int64_t frameUs = now - lastFrame;
        lastFrame = now;
        frameCount++;
        if (frameUs > 0) fps = 1000000.0f / frameUs;
        if (frameCount % 30 == 0) {
            _streamFps = fps;
            LOG_DEBUG("CAM", "stream: %.1f fps, frame %u", fps, frameCount);
        }
    }

    _clientActive = false;
    _streamFps = 0;
    LOG_INFO("CAM", "Stream client disconnected after %u frames", frameCount);
    return res;
}

// =============================================================================
// CAPTURE HANDLER
// =============================================================================

static esp_err_t capture_handler(httpd_req_t* req) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    esp_err_t res;
    if (fb->format == PIXFORMAT_JPEG) {
        res = httpd_resp_send(req, (const char*)fb->buf, fb->len);
    } else {
        uint8_t* jpegBuf = NULL;
        size_t jpegLen   = 0;
        if (frame2jpg(fb, CAM_STREAM_QUALITY, &jpegBuf, &jpegLen)) {
            res = httpd_resp_send(req, (const char*)jpegBuf, jpegLen);
            free(jpegBuf);
        } else {
            httpd_resp_send_500(req);
            res = ESP_FAIL;
        }
    }

    esp_camera_fb_return(fb);
    LOG_DEBUG("CAM", "Snapshot served");
    return res;
}

// =============================================================================
// initCamera()
// =============================================================================

bool initCamera() {
    LOG_INFO("CAM", "Initializing camera (OV2640/OV5640, SVGA, %dfps record)", CAM_RECORD_FPS_TARGET);

    _frameMutex = xSemaphoreCreateMutex();

    camera_config_t cfg = {};
    cfg.pin_pwdn     = CAM_PWDN_PIN;
    cfg.pin_reset    = -1;
    cfg.pin_xclk     = CAM_XCLK_PIN;
    cfg.pin_sccb_sda = CAM_SDA_PIN;
    cfg.pin_sccb_scl = CAM_SCL_PIN;
    cfg.pin_d7       = CAM_D7_PIN;
    cfg.pin_d6       = CAM_D6_PIN;
    cfg.pin_d5       = CAM_D5_PIN;
    cfg.pin_d4       = CAM_D4_PIN;
    cfg.pin_d3       = CAM_D3_PIN;
    cfg.pin_d2       = CAM_D2_PIN;
    cfg.pin_d1       = CAM_D1_PIN;
    cfg.pin_d0       = CAM_D0_PIN;
    cfg.pin_vsync    = CAM_VSYNC_PIN;
    cfg.pin_href     = CAM_HREF_PIN;
    cfg.pin_pclk     = CAM_PCLK_PIN;

    cfg.xclk_freq_hz = CAM_XCLK_FREQ_HZ;
    cfg.ledc_timer   = (ledc_timer_t)  CAM_LEDC_TIMER;
    cfg.ledc_channel = (ledc_channel_t)CAM_LEDC_CHANNEL;

    cfg.pixel_format = PIXFORMAT_JPEG;
    cfg.frame_size   = CAM_FRAME_SIZE;
    cfg.jpeg_quality = CAM_STREAM_QUALITY;
    cfg.fb_count     = 2;
    cfg.fb_location  = CAMERA_FB_IN_PSRAM;
    cfg.grab_mode    = CAMERA_GRAB_LATEST;

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        LOG_ERROR("CAM", "esp_camera_init failed: 0x%x", err);
        sensorData.cameraReady = false;
        sensorData.err |= ERR_CAMERA;
        return false;
    }

    sensor_t* s = esp_camera_sensor_get();
    if (s) {
        s->set_vflip(s, 1);
        s->set_quality(s, CAM_STREAM_QUALITY);
        s->set_framesize(s, CAM_FRAME_SIZE);
        if (s->id.PID == OV2640_PID) {
            s->set_whitebal(s, 1);
            s->set_gain_ctrl(s, 1);
            s->set_exposure_ctrl(s, 1);
        } else if (s->id.PID == OV5640_PID) {
            s->set_whitebal(s, 1);
            s->set_gain_ctrl(s, 1);
            s->set_exposure_ctrl(s, 1);
            s->set_aec2(s, 1);
        }
        LOG_INFO("CAM", "Sensor PID: 0x%04x (%s)",
                 s->id.PID,
                 s->id.PID == OV2640_PID ? "OV2640" :
                 s->id.PID == OV5640_PID ? "OV5640" : "unknown");
    }

    _camReady = true;
    sensorData.cameraReady = true;
    sensorData.err &= ~ERR_CAMERA;

    httpd_config_t hcfg   = HTTPD_DEFAULT_CONFIG();
    hcfg.server_port      = CAM_STREAM_PORT;
    hcfg.ctrl_port        = CAM_STREAM_PORT + 1000;
    hcfg.max_uri_handlers = 4;
    hcfg.stack_size       = 8192;

    if (httpd_start(&_streamServer, &hcfg) != ESP_OK) {
        LOG_ERROR("CAM", "Stream server failed to start on port %d", CAM_STREAM_PORT);
        return false;
    }

    httpd_uri_t stream_uri = {
        .uri     = "/stream",
        .method  = HTTP_GET,
        .handler = stream_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(_streamServer, &stream_uri);

    httpd_uri_t capture_uri = {
        .uri     = "/capture",
        .method  = HTTP_GET,
        .handler = capture_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(_streamServer, &capture_uri);

    LOG_INFO("CAM", "Stream server on port %d: /stream, /capture", CAM_STREAM_PORT);
    LOG_INFO("CAM", "Init OK: SVGA 800x600 JPEG, fb_count=2, PSRAM, record=%dfps", CAM_RECORD_FPS_TARGET);
    return true;
}

// =============================================================================
// deinitCamera()
// =============================================================================

void deinitCamera() {
    stopRecordingNow();
    if (_streamServer) {
        httpd_stop(_streamServer);
        _streamServer = NULL;
    }
    esp_camera_deinit();
    _camReady = false;
    sensorData.cameraReady = false;
    LOG_INFO("CAM", "Camera deinitialized");
}

// =============================================================================
// startMotionRecording()
// =============================================================================

void startMotionRecording() {
    if (!_camReady) return;

    lastMotionMs = millis();

    if (_recordActive) {
        LOG_DEBUG("CAM", "Recording extended by motion (lastMotionMs updated)");
        return;
    }

    if (!sdPresent) {
        LOG_WARN("CAM", "startMotionRecording: SD not present, skipping");
        return;
    }

    _recordActive  = true;
    _recordStartMs = millis();

    BaseType_t rc = xTaskCreatePinnedToCore(
        record_task,
        "record_task",
        12288,
        NULL,
        4,
        &_recordTask,
        1
    );

    if (rc != pdPASS) {
        LOG_ERROR("CAM", "Failed to create record_task");
        _recordActive = false;
        _recordTask   = NULL;
    } else {
        LOG_INFO("CAM", "Recording started (trigger: PIR motion)");
    }
}

// =============================================================================
// performMotionRecordingCheck()
// =============================================================================

void performMotionRecordingCheck() {
    if (!_recordActive) return;

    unsigned long now = millis();

    if (now - _recordStartMs >= (CAM_MAX_RECORD_SEC * 1000UL)) {
        LOG_INFO("CAM", "Recording: max duration (%ds) reached, stopping", CAM_MAX_RECORD_SEC);
        _recordActive = false;
        return;
    }

    if (now - (unsigned long)lastMotionMs >= (CAM_POST_MOTION_SEC * 1000UL)) {
        LOG_INFO("CAM", "Recording: no motion for %ds, stopping", CAM_POST_MOTION_SEC);
        _recordActive = false;
        return;
    }
}

// =============================================================================
// stopRecordingNow()
// =============================================================================

void stopRecordingNow() {
    if (!_recordActive) return;
    _recordActive = false;
    uint32_t wait = 0;
    while (_recordTask != NULL && wait < 5000) {
        vTaskDelay(pdMS_TO_TICKS(100));
        wait += 100;
    }
    if (_recordTask != NULL) {
        LOG_WARN("CAM", "record_task did not finish in 5s - force delete");
        vTaskDelete(_recordTask);
        _recordTask = NULL;
    }
    sensorData.isRecording = false;
}

// =============================================================================
// getCameraStatus()
// =============================================================================

CameraStatus getCameraStatus() {
    CameraStatus st = {};
    st.ready           = _camReady;
    st.streaming       = _clientActive;
    st.recording       = _recordActive;
    st.recordFrames    = sensorData.recordingFrames;
    st.recordSecs      = _recordActive ? (millis() - _recordStartMs) / 1000 : 0;
    st.streamFps       = _streamFps;
    st.lastFileSize    = _lastFileSize;
    st.totalRecordings = _totalRecordings;
    strncpy(st.lastFile, _lastFilename, sizeof(st.lastFile) - 1);
    st.lastFile[sizeof(st.lastFile) - 1] = '\0';
    return st;
}

bool isCameraReady()  { return _camReady; }
bool isRecording()    { return _recordActive; }
