// cam.h - Kamera modul za vent_SEW
//
// Hardware: OV2640 ali OV5640, DVP vmesnik
// Pini: glej config.h (CAM_* defines)
// SCCB I2C: IO16/IO21 (ločen bus od sensor/touch busov)
//
// Funkcionalnost:
//   1. MJPEG live stream  → port CAM_STREAM_PORT (81), endpoint /stream
//   2. JPEG snapshot      → port CAM_STREAM_PORT (81), endpoint /capture
//   3. AVI snemanje       → SD kartica, /recordings/YYYY-MM-DD_HH-MM-SS.avi
//      - Sproži PIR trigger iz sens.cpp
//      - Zaustavi se: N sek po zadnjem gibanju + absolutni max
//
// Task arhitektura (FreeRTOS):
//   stream_task  (Core 1, pri 5) → streama MJPEG ko je klient povezan
//   record_task  (Core 1, pri 4) → zapisuje AVI na SD med snemanjem
//   oba tasaka si delita frame_mutex za dostop do camera_fb_t
//
// Medsebojna odvisnost:
//   cam.cpp  ↔  globals.h  (sensorData.cameraReady, .isRecording, itd.)
//   cam.cpp  ↔  sd_card.cpp (sdMutex za SD dostop med AVI write)
//   cam.cpp  ↔  avi.h/cpp  (AVI container writer)
//   cam.cpp  ↔  logging.h  (LOG_INFO/WARN/ERROR)
//   sens.cpp → cam.cpp     (startMotionRecording() ob PIR trigger)
//   http.cpp → cam.cpp     (getCameraStatus() za /cam/status endpoint)

#ifndef CAM_H
#define CAM_H

#include "globals.h"
#include "esp_camera.h"

// --- Stanje kamere za zunanjo uporabo ---
struct CameraStatus {
    bool ready;              // esp_camera_init() uspešen
    bool streaming;          // trenutno aktiven MJPEG klient
    bool recording;          // AVI snemanje aktivno
    uint32_t recordFrames;   // število posnetih frameov v aktivnem posnetku
    uint32_t recordSecs;     // čas snemanja [s]
    float    streamFps;      // dejanski stream fps (povprečje zadnjih 5s)
    char     lastFile[40];   // zadnji AVI filename, npr. "2026-02-22_10-30-00.avi"
    uint32_t lastFileSize;   // velikost zadnjega AVI [bytes]
    uint32_t totalRecordings;// skupno število posnetkov od zadnjega restartu
};

// =============================================================================
// Inicializacija
// =============================================================================

// Inicializira kamero in zažene HTTP stream strežnik na port CAM_STREAM_PORT.
// Kliče se iz main setup(). Vrne false če kamera ni prisotna ali init ne uspe.
bool initCamera();

// Deinit (ob kritični napaki / OTA)
void deinitCamera();

// =============================================================================
// Snemanje (klic iz sens.cpp ob PIR trigger)
// =============================================================================

// Začne AVI snemanje. Varno za klic iz loop() ali ISR konteksta.
// Če snemanje že teče, samo posodobi lastMotionMs (podaljša snemanje).
void startMotionRecording();

// Kliče se vsak loop() cikel - preverja ali je čas za ustavitev snemanja.
void performMotionRecordingCheck();

// Prisilna ustavitev (npr. pred OTA update)
void stopRecordingNow();

// =============================================================================
// Status
// =============================================================================

CameraStatus getCameraStatus();
bool isCameraReady();
bool isRecording();

#endif // CAM_H
