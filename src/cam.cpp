// cam.cpp - Kamera modul za vent_SEW
//
// Inicializacija:    initCamera()      → esp_camera_init() + HTTP stream server
// Stream:            stream_task       → MJPEG multipart na port 81, /stream
// Snapshot:          capture_handler   → en JPEG frame na /capture
// Snemanje (AVI):    record_task       → sproži PIR, shrani na SD
//
// Task arhitektura:
//   stream_task  (Core 1, pri 5): teče ves čas, blokira na frame
//   record_task  (Core 1, pri 4): ustvari ob startMotionRecording(), self-delete ob koncu
//
// SPI / SD mutex:
//   SD kartica deli SPI bus z LCD (sdMutex iz globals.h).
//   record_task drži sdMutex samo med posameznim avi->writeFrame() klicem (~2ms).
//   Daljši hold (avi->open/close) se izvede z mutex timeout watchdog.
//
// PSRAM:
//   fb_location = CAMERA_FB_IN_PSRAM
//   fb_count    = 2 (double buffer za smooth stream + record hkrati)
//
// POPRAVKI (2026-02-28):
//   - Odstranjeni lokalni #define CAM_PIN_* (16 duplikatov config.h!)
//     Zamenjano z config.h makroji (CAM_PWDN_PIN, CAM_XCLK_PIN ...).
//
// POPRAVKI (2026-03-01):
//   - KRITIČNO: _lastMotionMs (lokalna static spremenljivka) zamenjana z
//     lastMotionMs (globalna iz globals.h, volatile).
//     Prej: sens.cpp je pisal v globals lastMotionMs, cam.cpp bral lokalno
//           _lastMotionMs → kamera ni nikoli upoštevala PIR eventov iz senzorja!
//           PIR je sicer sprožil startMotionRecording() prek sens.cpp→cam.cpp klic,
//           ampak performMotionRecordingCheck() ni videl obnove časa iz sens.cpp.
//           Posledica: snemanje se je ustavilo po CAM_POST_MOTION_SEC od
//           ZAČETKA snemanja (ne od zadnjega gibanja).
//   - LEDC kanal in timer sta zdaj iz config.h (CAM_LEDC_CHANNEL, CAM_LEDC_TIMER)
//     namesto hardcoded magic numbers LEDC_TIMER_2, LEDC_CHANNEL_2.
//   - OV5640_PID: zaščiten z #ifndef v config.h (varno za vse verzije esp32-camera)
//
// POPRAVKI (2026-03-01 v2):
//   - FIX KRITIČNO: Stack overflow v record_task!
//     AviWriter vsebuje _index[AVI_MAX_FRAMES=900] × 16B = 14.400 B na stack-u
//     taska z le 8.192 B → crash ob prvem snemanju.
//     Rešitev: AviWriter se zdaj alocira na heap (new/delete) znotraj record_task.
//     Stack record_task povečan iz 8192 na 12288 B (za sam task overhead).

#include "cam.h"
#include "avi.h"
#include "globals.h"
#include "logging.h"
#include "sd_card.h"
#include <esp_camera.h>
#include <esp_http_server.h>
#include <esp_timer.h>

// =============================================================================
// KONFIGURACIJA (pini iz config.h, LEDC iz config.h)
// =============================================================================

// Stream / snemanje parametri
#define CAM_STREAM_PORT         81
#define CAM_XCLK_FREQ_HZ        20000000
#define CAM_FRAME_SIZE          FRAMESIZE_SVGA   // 800x600
#define CAM_STREAM_QUALITY      12               // JPEG quality 0-63
#define CAM_RECORD_QUALITY      10               // Malo boljša kakovost za snemanje
#define CAM_RECORD_FPS_TARGET    7               // Target fps (SVGA ~7fps)
#define CAM_POST_MOTION_SEC     10               // Sekund po zadnjem gibanju
#define CAM_MAX_RECORD_SEC     120               // Absolutni max snemanja
#define CAM_RECORD_DIR          "/recordings"    // SD mapa
#define CAM_RECORD_FRAME_MS     (1000 / CAM_RECORD_FPS_TARGET)  // ~143ms med framei

// MJPEG stream boundary
#define STREAM_BOUNDARY         "vent_sew_stream"
#define STREAM_CONTENT_TYPE     "multipart/x-mixed-replace;boundary=" STREAM_BOUNDARY
#define STREAM_BOUNDARY_LINE    "\r\n--" STREAM_BOUNDARY "\r\n"
#define STREAM_PART_HDR         "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n"

// =============================================================================
// INTERNE SPREMENLJIVKE
// =============================================================================

static bool              _camReady       = false;
static httpd_handle_t    _streamServer   = NULL;
static SemaphoreHandle_t _frameMutex     = NULL;
static TaskHandle_t      _recordTask     = NULL;

// Snemanje state
static volatile bool     _recordActive   = false;
// FIX (2026-03-01): _lastMotionMs ODSTRANJENA - namesto nje se bere globals lastMotionMs
// Razlog: sens.cpp piše lastMotionMs ob PIR zaznavi, ta modul mora brati isto spremenljivko!
// static volatile unsigned long _lastMotionMs = 0;  // ODSTRANJENO!
static volatile unsigned long _recordStartMs  = 0;

// Statistike
static volatile uint32_t _totalRecordings = 0;
static volatile uint32_t _lastFileSize    = 0;
static char              _lastFilename[40] = "";

// Stream fps tracking
static volatile float    _streamFps     = 0.0f;
static volatile bool     _clientActive  = false;

// =============================================================================
// POMOZNE FUNKCIJE
// =============================================================================

static void buildFilename(char* buf, size_t bufLen) {
    if (timeSynced) {
        snprintf(buf, bufLen, "%s/%s.avi",
                 CAM_RECORD_DIR,
                 myTZ.dateTime("Y-m-d_H-i-s").c_str());
    } else {
        snprintf(buf, bufLen, "%s/rec_%lu.avi",
                 CAM_RECORD_DIR, millis() / 1000);
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
// RECORD TASK
//
// FIX (2026-03-01 v2): AviWriter alociran na HEAP namesto na stack!
//
// Razlog: AviWriter._index[AVI_MAX_FRAMES] = 900 × 16B = 14.400B.
//   Ko je AviWriter lokalna spremenljivka v record_task, je 14.400B na
//   task stack-u. record_task ima stack 8.192B → takoj ob kreaciji AviWriter
//   nastopi stack overflow → CPU exception → reboot.
//
// Rešitev: new AviWriter() alocira objekt na PSRAM/heap (ESP32-S3R8 ima 8MB PSRAM).
//   - Task stack zmanjšamo na 12.288B (dovolj za task overhead + lokalne spremenljivke)
//   - delete avi se izvede na koncu ali ob vsaki poti napake
//   - Null check po new zagotovi varno ravnanje ob OOM
// =============================================================================

static void record_task(void* arg) {
    LOG_INFO("CAM", "Record task started");

    // FIX: AviWriter NA HEAP - ne na stack (14.400B bi prekoračilo 8192B stack)!
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

    LOG_INFO("CAM", "Recording: %s", filename);

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

    // FIX: sprostimo heap alokacijo
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
    LOG_INFO("CAM", "Initializing camera (OV2640/OV5640, SVGA)");

    _frameMutex = xSemaphoreCreateMutex();

    // Pini iz config.h (CAM_*_PIN) - brez lokalnih duplikatov!
    // LEDC kanal/timer iz config.h (CAM_LEDC_CHANNEL, CAM_LEDC_TIMER) - brez magic numbers!
    camera_config_t cfg = {};
    cfg.pin_pwdn     = CAM_PWDN_PIN;    // IO17
    cfg.pin_reset    = -1;              // Ni reset pina
    cfg.pin_xclk     = CAM_XCLK_PIN;   // IO8
    cfg.pin_sccb_sda = CAM_SDA_PIN;    // IO21
    cfg.pin_sccb_scl = CAM_SCL_PIN;    // IO16
    cfg.pin_d7       = CAM_D7_PIN;     // IO2
    cfg.pin_d6       = CAM_D6_PIN;     // IO7
    cfg.pin_d5       = CAM_D5_PIN;     // IO10
    cfg.pin_d4       = CAM_D4_PIN;     // IO14
    cfg.pin_d3       = CAM_D3_PIN;     // IO11
    cfg.pin_d2       = CAM_D2_PIN;     // IO15
    cfg.pin_d1       = CAM_D1_PIN;     // IO13
    cfg.pin_d0       = CAM_D0_PIN;     // IO12
    cfg.pin_vsync    = CAM_VSYNC_PIN;  // IO6
    cfg.pin_href     = CAM_HREF_PIN;   // IO4
    cfg.pin_pclk     = CAM_PCLK_PIN;  // IO9

    cfg.xclk_freq_hz = CAM_XCLK_FREQ_HZ;

    // FIX (2026-03-01): LEDC iz config.h namesto hardcoded LEDC_TIMER_2/LEDC_CHANNEL_2
    // CAM_LEDC_TIMER = 2, CAM_LEDC_CHANNEL = 2 (različno od LEDC_BL_CHANNEL=0!)
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
            // OV5640_PID je zaščiten z #ifndef v config.h - varno za vse verzije esp32-camera
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
    LOG_INFO("CAM", "Init OK: SVGA 800x600 JPEG, fb_count=2, PSRAM");
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
// FIX (2026-03-01): ob vsakem klicu se posodobi globals lastMotionMs
//   (namesto lokalne _lastMotionMs ki jo sens.cpp ni mogel posodabljati)
// =============================================================================

void startMotionRecording() {
    if (!_camReady) return;

    // FIX: posodobi globalno lastMotionMs, ki jo record_task bere v
    // performMotionRecordingCheck(). sens.cpp posodablja lastMotionMs ob PIR,
    // ta klic jo posodobi ob ročnem zagonu snemanja.
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

    // FIX (2026-03-01 v2): stack povečan na 12288B
    // AviWriter je zdaj na heap (new/delete v record_task), ne na stack.
    // 12288B zadošča za task overhead, lokalne spremenljivke, frame processing.
    BaseType_t rc = xTaskCreatePinnedToCore(
        record_task,
        "record_task",
        12288,          // FIX: 8192 → 12288 (AviWriter je na heap, ne tukaj)
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
// FIX (2026-03-01): bere globals lastMotionMs (volatile) namesto _lastMotionMs
//   lastMotionMs posodablja sens.cpp ob vsakem PIR eventu → pravilno podaljševanje
// =============================================================================

void performMotionRecordingCheck() {
    if (!_recordActive) return;

    unsigned long now = millis();

    // Absolutni maksimum snemanja
    if (now - _recordStartMs >= (CAM_MAX_RECORD_SEC * 1000UL)) {
        LOG_INFO("CAM", "Recording: max duration (%ds) reached, stopping", CAM_MAX_RECORD_SEC);
        _recordActive = false;
        return;
    }

    // FIX: lastMotionMs (globals, volatile) - sens.cpp ga posodablja ob PIR
    // Prej: _lastMotionMs (lokalna) - sens.cpp je NI posodabljal → napačno ustavitev
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
