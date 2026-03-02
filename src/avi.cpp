// avi.cpp - AVI MJPEG container writer za vent_SEW

#include "avi.h"
#include "logging.h"
#include <SD.h>

// =============================================================================
// Konstruktor / Destruktor
// =============================================================================

AviWriter::AviWriter()
    : _width(0), _height(0), _fps(0), _frameCount(0), _fileSize(0),
      _avihFrameCountOffset(0), _strhLengthOffset(0), _moviDataOffset(0)
{}

AviWriter::~AviWriter() {
    if (_file) close();
}

// =============================================================================
// Pomožne write funkcije
// =============================================================================

void AviWriter::writeFourCC(const char* cc) {
    _file.write((const uint8_t*)cc, 4);
    _fileSize += 4;
}

void AviWriter::writeU32(uint32_t v) {
    _file.write((const uint8_t*)&v, 4);
    _fileSize += 4;
}

void AviWriter::writeU16(uint16_t v) {
    _file.write((const uint8_t*)&v, 2);
    _fileSize += 2;
}

void AviWriter::writeBytes(const void* data, size_t len) {
    _file.write((const uint8_t*)data, len);
    _fileSize += len;
}

// =============================================================================
// open()
// =============================================================================

bool AviWriter::open(const char* filename, uint16_t width, uint16_t height, uint8_t fps) {
    if (_file) {
        LOG_WARN("AVI", "open() called while already open - closing first");
        close();
    }

    _width      = width;
    _height     = height;
    _fps        = fps > 0 ? fps : 7;
    _frameCount = 0;
    _fileSize   = 0;

    _file = SD.open(filename, FILE_WRITE);
    if (!_file) {
        LOG_ERROR("AVI", "Cannot open %s for writing", filename);
        return false;
    }

    uint32_t usPerFrame = 1000000UL / _fps;

    // -------------------------------------------------------------------------
    // RIFF 'AVI '
    // -------------------------------------------------------------------------
    writeFourCC("RIFF");
    uint32_t riffSizeOffset = _fileSize;
    writeU32(0);               // placeholder - posodobimo ob close()
    writeFourCC("AVI ");

    // -------------------------------------------------------------------------
    // LIST 'hdrl'
    // -------------------------------------------------------------------------
    writeFourCC("LIST");
    writeU32(                  // hdrl chunk size (fiksno za ta format)
        4 +                    // 'hdrl'
        8 + sizeof(AviMainHeader) +   // 'avih' chunk
        4 + 8 + sizeof(AviStreamHeader) + 8 + sizeof(BitmapInfoHeader) + 4  // LIST strl
    );
    writeFourCC("hdrl");

    // --- avih (AVI Main Header) ---
    writeFourCC("avih");
    writeU32(sizeof(AviMainHeader));
    _avihFrameCountOffset = _fileSize + offsetof(AviMainHeader, dwTotalFrames);

    AviMainHeader avih = {};
    avih.dwMicroSecPerFrame    = usPerFrame;
    avih.dwMaxBytesPerSec      = (uint32_t)(_width * _height * 3 * _fps / 2); // estiamte
    avih.dwFlags               = 0x10;  // AVIF_HASINDEX
    avih.dwTotalFrames         = 0;     // posodobimo ob close
    avih.dwStreams             = 1;
    avih.dwSuggestedBufferSize = _width * _height * 3;
    avih.dwWidth               = _width;
    avih.dwHeight              = _height;
    writeBytes(&avih, sizeof(avih));

    // --- LIST 'strl' ---
    writeFourCC("LIST");
    writeU32(
        4 +
        8 + sizeof(AviStreamHeader) +
        8 + sizeof(BitmapInfoHeader)
    );
    writeFourCC("strl");

    // --- strh (Stream Header) ---
    writeFourCC("strh");
    writeU32(sizeof(AviStreamHeader));
    _strhLengthOffset = _fileSize + offsetof(AviStreamHeader, dwLength);

    AviStreamHeader strh = {};
    memcpy(strh.fccType,    "vids", 4);
    memcpy(strh.fccHandler, "MJPG", 4);
    strh.dwScale               = 1;
    strh.dwRate                = _fps;
    strh.dwSuggestedBufferSize = _width * _height * 3;
    strh.dwQuality             = 0xFFFFFFFF;
    strh.rcFrame.right         = (int16_t)_width;
    strh.rcFrame.bottom        = (int16_t)_height;
    strh.dwLength              = 0;  // posodobimo ob close
    writeBytes(&strh, sizeof(strh));

    // --- strf (Stream Format = BITMAPINFOHEADER) ---
    writeFourCC("strf");
    writeU32(sizeof(BitmapInfoHeader));

    BitmapInfoHeader strf = {};
    strf.biSize        = sizeof(BitmapInfoHeader);
    strf.biWidth       = _width;
    strf.biHeight      = _height;
    strf.biPlanes      = 1;
    strf.biBitCount    = 24;
    strf.biCompression = 0x47504A4D;  // 'MJPG'
    strf.biSizeImage   = _width * _height * 3;
    writeBytes(&strf, sizeof(strf));

    // -------------------------------------------------------------------------
    // LIST 'movi' - frami se appendajo tukaj
    // -------------------------------------------------------------------------
    writeFourCC("LIST");
    uint32_t moviSizeOffset = _fileSize;
    writeU32(0);               // placeholder - posodobimo ob close()
    writeFourCC("movi");
    _moviDataOffset = _fileSize;  // offset začetka movi data (za idx1 offsets)

    _file.flush();
    LOG_INFO("AVI", "Opened %s (%ux%u @ %dfps), header=%u B",
             filename, _width, _height, _fps, _fileSize);

    (void)riffSizeOffset;   // bomo posodobili ob close() z seek
    (void)moviSizeOffset;
    return true;
}

// =============================================================================
// writeFrame()
// =============================================================================

bool AviWriter::writeFrame(const uint8_t* jpegData, size_t jpegLen) {
    if (!_file) return false;
    if (_frameCount >= AVI_MAX_FRAMES) {
        LOG_WARN("AVI", "Max frames (%d) reached - ignoring frame", AVI_MAX_FRAMES);
        return false;
    }

    // Chunk offset za idx1 (relativno od začetka movi data)
    uint32_t chunkOffset = _fileSize - _moviDataOffset;

    // Zapišemo '00dc' chunk (JPEG frame)
    writeFourCC("00dc");
    writeU32((uint32_t)jpegLen);
    writeBytes(jpegData, jpegLen);

    // JPEG chunki morajo biti word-aligned (padamo na sod bajt)
    if (jpegLen & 1) {
        uint8_t pad = 0;
        _file.write(&pad, 1);
        _fileSize++;
    }

    // Shranimo v index
    _index[_frameCount].ckid[0] = '0';
    _index[_frameCount].ckid[1] = '0';
    _index[_frameCount].ckid[2] = 'd';
    _index[_frameCount].ckid[3] = 'c';
    _index[_frameCount].dwFlags       = 0x10;  // AVIIF_KEYFRAME (MJPEG so vsi keyframe)
    _index[_frameCount].dwChunkOffset = chunkOffset;
    _index[_frameCount].dwChunkSize   = (uint32_t)jpegLen;

    _frameCount++;
    return true;
}

// =============================================================================
// close()
// =============================================================================

bool AviWriter::close() {
    if (!_file) return false;

    // -------------------------------------------------------------------------
    // Zapiši idx1 (index vseh frameov)
    // -------------------------------------------------------------------------
    uint32_t idx1Offset = _fileSize;
    writeFourCC("idx1");
    writeU32(_frameCount * sizeof(AviIndexEntry));
    for (uint32_t i = 0; i < _frameCount; i++) {
        writeBytes(&_index[i], sizeof(AviIndexEntry));
    }

    uint32_t finalFileSize = _fileSize;

    // -------------------------------------------------------------------------
    // Posodobi placeholderje z dejanskimi vrednostmi (seek + write)
    // -------------------------------------------------------------------------

    // RIFF chunk size = fileSize - 8 (RIFF header)
    _file.seek(4);
    uint32_t riffSize = finalFileSize - 8;
    _file.write((const uint8_t*)&riffSize, 4);

    // avih.dwTotalFrames
    _file.seek(_avihFrameCountOffset);
    _file.write((const uint8_t*)&_frameCount, 4);

    // strh.dwLength (frame count)
    _file.seek(_strhLengthOffset);
    _file.write((const uint8_t*)&_frameCount, 4);

    // movi LIST size = (fileSize - idx1 chunk) - movi LIST header (8 bytes) - 4 ('movi' fourcc)
    // = idx1Offset - _moviDataOffset + 4 (za 'movi' fourcc)
    uint32_t moviListSize = idx1Offset - (_moviDataOffset - 4); // -4 za 'movi'
    _file.seek(_moviDataOffset - 8);  // pred LIST size field
    _file.write((const uint8_t*)&moviListSize, 4);

    _file.flush();
    _file.close();

    float durationSec = _fps > 0 ? (float)_frameCount / _fps : 0;
    LOG_INFO("AVI", "Closed: %u frames, %.1fs, %.1f fps actual, %u B",
             _frameCount, durationSec,
             durationSec > 0 ? _frameCount / durationSec : 0,
             finalFileSize);

    _frameCount = 0;
    _fileSize   = 0;
    return true;
}

// =============================================================================
// durationSec()
// =============================================================================

uint32_t AviWriter::durationSec() const {
    if (_fps == 0) return 0;
    return _frameCount / _fps;
}
