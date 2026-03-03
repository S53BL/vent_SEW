// avi.h - AVI MJPEG container writer za vent_SEW
//
// SPREMEMBA (2026-03-03): AVI_MAX_FRAMES zmanjšan iz 900 na 400
//   Prej: 7fps × 120s = 840 → vzeto 900 → _index heap: 14.400B
//   Zdaj: 3fps × 120s = 360 → vzeto 400 → _index heap:  6.400B
//   Prihranek: ~8KB na heap alokaciji AviWriter objekta

#ifndef AVI_H
#define AVI_H

#include <Arduino.h>
#include "sd.h"
#include <stdint.h>

// Max število frameov v idx1 indexu
// 3fps × 120s max = 360 frameov; vzamemo 400 za rezervo
#define AVI_MAX_FRAMES  400

#pragma pack(push, 1)

struct AviMainHeader {
    uint32_t dwMicroSecPerFrame;
    uint32_t dwMaxBytesPerSec;
    uint32_t dwPaddingGranularity;
    uint32_t dwFlags;
    uint32_t dwTotalFrames;
    uint32_t dwInitialFrames;
    uint32_t dwStreams;
    uint32_t dwSuggestedBufferSize;
    uint32_t dwWidth;
    uint32_t dwHeight;
    uint32_t dwReserved[4];
};

struct AviStreamHeader {
    char     fccType[4];
    char     fccHandler[4];
    uint32_t dwFlags;
    uint16_t wPriority;
    uint16_t wLanguage;
    uint32_t dwInitialFrames;
    uint32_t dwScale;
    uint32_t dwRate;
    uint32_t dwStart;
    uint32_t dwLength;
    uint32_t dwSuggestedBufferSize;
    uint32_t dwQuality;
    uint32_t dwSampleSize;
    struct { int16_t left, top, right, bottom; } rcFrame;
};

struct BitmapInfoHeader {
    uint32_t biSize;
    int32_t  biWidth;
    int32_t  biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
};

struct AviIndexEntry {
    char     ckid[4];
    uint32_t dwFlags;
    uint32_t dwChunkOffset;
    uint32_t dwChunkSize;
};

#pragma pack(pop)

class AviWriter {
public:
    AviWriter();
    ~AviWriter();

    bool open(const char* filename, uint16_t width, uint16_t height, uint8_t fps);
    bool writeFrame(const uint8_t* jpegData, size_t jpegLen);
    bool close();

    bool     isOpen()      const { return (bool)_file; }
    uint32_t frameCount()  const { return _frameCount; }
    uint32_t fileSize()    const { return _fileSize; }
    uint32_t durationSec() const;

private:
    ::File   _file;
    uint16_t _width;
    uint16_t _height;
    uint8_t  _fps;
    uint32_t _frameCount;
    uint32_t _fileSize;
    uint32_t _avihFrameCountOffset;
    uint32_t _strhLengthOffset;
    uint32_t _moviDataOffset;
    AviIndexEntry _index[AVI_MAX_FRAMES];

    void writeFourCC(const char* cc);
    void writeU32(uint32_t v);
    void writeU16(uint16_t v);
    void writeBytes(const void* data, size_t len);
};

#endif // AVI_H
