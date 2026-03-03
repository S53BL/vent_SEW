// graph_store.cpp - LittleFS ring buffer za zgodovino grafov (vent_SEW)

#include "graph_store.h"
#include "logging.h"

// -----------------------------------------------------------------------
// RAM buffer
// -----------------------------------------------------------------------
GraphPoint gsHistory[GRAPH_STORE_MAX_POINTS];
int        gsHead  = 0;
int        gsCount = 0;

// -----------------------------------------------------------------------
// Interne pomožne funkcije
// -----------------------------------------------------------------------

// Izračuna file offset za zapis pri danem fizičnem indeksu
static uint32_t _pointOffset(int physIdx) {
    return (uint32_t)(GRAPH_STORE_DATA_OFFSET + physIdx * (int)sizeof(GraphPoint));
}

// Zapiše glavo (head + count) v datoteko na pozicijo 4
// magic se zapiše samo ob ustvarjanju — ne prepisujemo ga ob vsaki točki
static bool _writeHeader(File& f, uint32_t head, uint32_t count) {
    if (!f.seek(4)) return false;
    uint32_t buf[2] = { head, count };
    return f.write((uint8_t*)buf, 8) == 8;
}

// -----------------------------------------------------------------------
// graphStoreInit
// -----------------------------------------------------------------------
bool graphStoreInit() {
    // LittleFS.begin() je idempotent — varno klicati večkrat
    if (!LittleFS.begin(true)) {
        LOG_ERROR("GSTORE", "LittleFS.begin() failed");
        return false;
    }

    // Preveri ali datoteka obstaja in ima pravilno velikost
    const size_t expectedSize = GRAPH_STORE_HEADER_SIZE
                              + GRAPH_STORE_MAX_POINTS * sizeof(GraphPoint);

    if (LittleFS.exists(GRAPH_STORE_FILE)) {
        File f = LittleFS.open(GRAPH_STORE_FILE, "r");
        if (!f) {
            LOG_ERROR("GSTORE", "Cannot open existing %s", GRAPH_STORE_FILE);
            return false;
        }
        size_t sz = f.size();
        uint32_t magic = 0;
        f.read((uint8_t*)&magic, 4);
        f.close();

        if (sz != expectedSize || magic != GRAPH_STORE_MAGIC) {
            LOG_WARN("GSTORE", "Invalid file (sz=%u magic=%08X) — recreating", sz, magic);
            LittleFS.remove(GRAPH_STORE_FILE);
        } else {
            LOG_INFO("GSTORE", "File OK (%u bytes)", sz);
            return true;
        }
    }

    // Ustvari novo datoteko z ničelnimi podatki
    File f = LittleFS.open(GRAPH_STORE_FILE, "w");
    if (!f) {
        LOG_ERROR("GSTORE", "Cannot create %s", GRAPH_STORE_FILE);
        return false;
    }

    // Zapiši magic
    uint32_t magic = GRAPH_STORE_MAGIC;
    f.write((uint8_t*)&magic, 4);

    // Zapiši head=0, count=0
    uint32_t zero = 0;
    f.write((uint8_t*)&zero, 4);
    f.write((uint8_t*)&zero, 4);

    // Zapiši prazne točke
    GraphPoint emptyPt = {};
    for (int i = 0; i < GRAPH_STORE_MAX_POINTS; i++) {
        f.write((uint8_t*)&emptyPt, sizeof(GraphPoint));
    }
    f.close();

    LOG_INFO("GSTORE", "Created new %s (%u bytes)", GRAPH_STORE_FILE, expectedSize);
    return true;
}

// -----------------------------------------------------------------------
// graphStoreLoad
// -----------------------------------------------------------------------
void graphStoreLoad() {
    gsHead  = 0;
    gsCount = 0;
    memset(gsHistory, 0, sizeof(gsHistory));

    File f = LittleFS.open(GRAPH_STORE_FILE, "r");
    if (!f) {
        LOG_ERROR("GSTORE", "Cannot open %s for read", GRAPH_STORE_FILE);
        return;
    }

    // Preberi glavo: preskoci magic (4), preberi head in count
    f.seek(4);
    uint32_t head = 0, count = 0;
    f.read((uint8_t*)&head,  4);
    f.read((uint8_t*)&count, 4);

    // Validacija
    if (head  >= (uint32_t)GRAPH_STORE_MAX_POINTS) head  = 0;
    if (count >  (uint32_t)GRAPH_STORE_MAX_POINTS) count = 0;

    gsHead  = (int)head;
    gsCount = (int)count;

    if (gsCount == 0) {
        f.close();
        LOG_INFO("GSTORE", "No data in %s", GRAPH_STORE_FILE);
        return;
    }

    // Preberi vse točke
    f.seek(GRAPH_STORE_DATA_OFFSET);
    f.read((uint8_t*)gsHistory, GRAPH_STORE_MAX_POINTS * sizeof(GraphPoint));
    f.close();

    LOG_INFO("GSTORE", "Loaded %d points (head=%d)", gsCount, gsHead);
}

// -----------------------------------------------------------------------
// graphStoreAdd
// -----------------------------------------------------------------------
void graphStoreAdd(const GraphPoint& pt) {
    int physIdx;

    if (gsCount < GRAPH_STORE_MAX_POINTS) {
        // Buffer še ni poln — dodamo na konec
        physIdx = (gsHead + gsCount) % GRAPH_STORE_MAX_POINTS;
        gsHistory[physIdx] = pt;
        gsCount++;
    } else {
        // Buffer poln — prepiši najstarejšo točko
        physIdx = gsHead;
        gsHistory[physIdx] = pt;
        gsHead = (gsHead + 1) % GRAPH_STORE_MAX_POINTS;
    }

    // Zapiši točko v LittleFS (samo ta ena točka + posodobi glavo)
    File f = LittleFS.open(GRAPH_STORE_FILE, "r+");
    if (!f) {
        LOG_ERROR("GSTORE", "Cannot open %s for write", GRAPH_STORE_FILE);
        return;
    }

    // Zapiši točko
    if (!f.seek(_pointOffset(physIdx))) {
        LOG_ERROR("GSTORE", "Seek failed for physIdx=%d", physIdx);
        f.close();
        return;
    }
    f.write((uint8_t*)&pt, sizeof(GraphPoint));

    // Posodobi glavo
    _writeHeader(f, (uint32_t)gsHead, (uint32_t)gsCount);

    f.close();
}

// -----------------------------------------------------------------------
// graphStoreGet
// -----------------------------------------------------------------------
const GraphPoint* graphStoreGet(int idx) {
    if (idx < 0 || idx >= gsCount) return nullptr;
    int physIdx = (gsHead + idx) % GRAPH_STORE_MAX_POINTS;
    return &gsHistory[physIdx];
}

// -----------------------------------------------------------------------
// graphStoreClear
// -----------------------------------------------------------------------
void graphStoreClear() {
    gsHead  = 0;
    gsCount = 0;
    memset(gsHistory, 0, sizeof(gsHistory));
    LittleFS.remove(GRAPH_STORE_FILE);
    graphStoreInit();
    LOG_INFO("GSTORE", "Store cleared");
}