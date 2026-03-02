// avi.h - AVI MJPEG container writer za vent_SEW
//
// Format: AVI 1.0, en video stream, MJPEG kompresija
// Kompatibilnost: VLC, Windows Media Player, ffmpeg
//
// Uporaba:
//   AviWriter avi;
//   avi.open("/recordings/2026-02-22_10-30-00.avi", 800, 600, 7);
//   avi.writeFrame(fb->buf, fb->len);   // ponovi za vsak frame
//   avi.close();                        // finalizira header
//
// Struktura AVI datoteke:
//   RIFF 'AVI '
//     LIST 'hdrl'  ← header (zapisan takoj)
//       avih       ← main AVI header (frame count se posodobi ob close)
//       LIST 'strl'
//         strh     ← stream header
//         strf     ← stream format (BITMAPINFOHEADER za MJPEG)
//     LIST 'movi'  ← začetek ob open(), frami se appendajo tukaj
//       00dc <size> <JPEG data>
//       00dc <size> <JPEG data>
//       ...
//     idx1         ← index (zapisan ob close())
//
// Opomba o SD mutex:
//   AviWriter sam NE drži sdMutex med posameznimi writeFrame() klici.
//   Klic mora biti zavarovan z sdMutex v cam.cpp (record_task).
//   open() in close() sta počasnejši operaciji - sdMutex se drži dlje.

#ifndef AVI_H
#define AVI_H

#include <Arduino.h>
#include "sd.h"
#include <stdint.h>

// Max število frameov v idx1 indexu
// 7fps × 120s max = 840 frameov; vzamemo 900 za rezervo
#define AVI_MAX_FRAMES  900

// =============================================================================
// AVI header strukture (little-endian, packed)
// =============================================================================

#pragma pack(push, 1)

struct AviMainHeader {
    uint32_t dwMicroSecPerFrame;  // 1000000 / fps
    uint32_t dwMaxBytesPerSec;
    uint32_t dwPaddingGranularity;
    uint32_t dwFlags;             // 0x10 = AVIF_HASINDEX
    uint32_t dwTotalFrames;       // posodobi ob close()
    uint32_t dwInitialFrames;
    uint32_t dwStreams;           // = 1
    uint32_t dwSuggestedBufferSize;
    uint32_t dwWidth;
    uint32_t dwHeight;
    uint32_t dwReserved[4];
};

struct AviStreamHeader {
    char     fccType[4];          // "vids"
    char     fccHandler[4];       // "MJPG"
    uint32_t dwFlags;
    uint16_t wPriority;
    uint16_t wLanguage;
    uint32_t dwInitialFrames;
    uint32_t dwScale;             // = 1
    uint32_t dwRate;              // = fps
    uint32_t dwStart;
    uint32_t dwLength;            // posodobi ob close()
    uint32_t dwSuggestedBufferSize;
    uint32_t dwQuality;
    uint32_t dwSampleSize;
    struct { int16_t left, top, right, bottom; } rcFrame;
};

struct BitmapInfoHeader {
    uint32_t biSize;              // = 40
    int32_t  biWidth;
    int32_t  biHeight;
    uint16_t biPlanes;            // = 1
    uint16_t biBitCount;          // = 24
    uint32_t biCompression;       // 'MJPG' = 0x47504A4D
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
};

struct AviIndexEntry {
    char     ckid[4];             // "00dc"
    uint32_t dwFlags;             // 0x10 = AVIIF_KEYFRAME
    uint32_t dwChunkOffset;       // offset od začetka movi LIST data
    uint32_t dwChunkSize;
};

#pragma pack(pop)

// =============================================================================
// AviWriter razred
// =============================================================================

class AviWriter {
public:
    AviWriter();
    ~AviWriter();

    // Odpre AVI datoteko, zapiše header skeleton.
    // width, height: resolucija (mora se ujemati s frameovi)
    // fps: ciljni fps (vpliva na dwMicroSecPerFrame in dwRate)
    // Vrne false če datoteka ne more biti odprta.
    bool open(const char* filename, uint16_t width, uint16_t height, uint8_t fps);

    // Doda JPEG frame v movi seznam. JPEG data mora biti veljaven JPEG.
    // Vrne false ob SD write napaki.
    bool writeFrame(const uint8_t* jpegData, size_t jpegLen);

    // Finalizira AVI: zapiše idx1 index, posodobi frame count v headerju.
    // Po close() je objekt pripravljen za ponovni open().
    bool close();

    // Ali je datoteka odprta?
    bool isOpen() const { return (bool)_file; }

    // Statistike
    uint32_t frameCount()  const { return _frameCount; }
    uint32_t fileSize()    const { return _fileSize; }
    uint32_t durationSec() const;

private:
    ::File     _file;
    uint16_t _width;
    uint16_t _height;
    uint8_t  _fps;
    uint32_t _frameCount;
    uint32_t _fileSize;

    // Offset v datoteki kjer je avih.dwTotalFrames (za posodobitev ob close)
    uint32_t _avihFrameCountOffset;
    // Offset kjer je strh.dwLength
    uint32_t _strhLengthOffset;
    // Offset začetka movi LIST data (za idx1 chunk offsets)
    uint32_t _moviDataOffset;

    // Index frameov (za idx1)
    AviIndexEntry _index[AVI_MAX_FRAMES];

    // Pomožne write funkcije
    void writeFourCC(const char* cc);
    void writeU32(uint32_t v);
    void writeU16(uint16_t v);
    void writeBytes(const void* data, size_t len);
};

#endif // AVI_H
