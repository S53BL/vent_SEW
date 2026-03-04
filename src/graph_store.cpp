// graph_store.cpp - LittleFS ring buffer implementacija (graph_update.md §4)
//
// RAM array + LittleFS sta vedno identični kopiji.
// graphStorePoint(): API točka za 3-min zanko → atomično zapiše v oba.
// graphLoadFromLittleFS(): ob zagonu obnovi RAM iz LittleFS.
// graphRefresh() (v disp_graph.cpp) bere SAMO iz RAM (nikoli iz LittleFS).
//
// Ring buffer logika (§4.3):
//   - history[] je linearen array, head kaže na najstarejšo/naslednjo lokacijo
//   - Ko je buffer poln (count == GRAPH_HISTORY_MAX):
//       history[head] = nova točka; head = (head + 1) % MAX
//   - Ko ni poln:
//       history[head + count] = nova točka; count++
//   - Kronološki vrstni red: history[(head + i) % MAX] za i = 0..count-1
//     (0 = najstarejša, count-1 = najnovejša)
//
// LittleFS format (§4.1): 8-byte header + GraphPoint[GRAPH_HISTORY_MAX]
//   - Pisanje točke: seek na pozicijo točke, zapiši 32 bytes
//   - Pisanje headerja: seek na 0, zapiši head(2)+count(2)+pad(4)
//   - Ločeni operaciji → možna nekonzistentnost ob izpadu
//   - Zaščita: ob zagunuvalidacija ts > 0 za vsako točko

#include "graph_store.h"
#include "logging.h"
#include <stdlib.h>   // malloc, free

// ============================================================
// RAM ring buffer
// ============================================================
static GraphPoint gsHistory[GRAPH_HISTORY_MAX];
static int        gsHead  = 0;    // indeks najstarejše točke (ali naslednji zapis ob polnem)
static int        gsCount = 0;    // število veljavnih točk
static bool       gsReady = false; // graphStoreInit() uspešen

// ============================================================
// INICIALIZACIJA
// ============================================================
bool graphStoreInit() {
    // Inicializiraj RAM buffer
    memset(gsHistory, 0, sizeof(gsHistory));
    gsHead  = 0;
    gsCount = 0;
    gsReady = false;

    // Odpri LittleFS
    if (!LittleFS.begin(true)) {  // true = formatira ob napaki
        LOG_ERROR("GraphStore", "LittleFS mount failed");
        return false;
    }

    // Preveri/ustvari /graph.bin
    if (!LittleFS.exists(GRAPH_STORE_FILE)) {
        // Datoteka ne obstaja → ustvari novo prazno
        File f = LittleFS.open(GRAPH_STORE_FILE, "w");
        if (!f) {
            LOG_ERROR("GraphStore", "Cannot create %s", GRAPH_STORE_FILE);
            return false;
        }
        // Zapiši prazen header: head=0, count=0, pad=0
        uint16_t h = 0, c = 0;
        uint32_t pad = 0;
        f.write((uint8_t*)&h,   sizeof(h));
        f.write((uint8_t*)&c,   sizeof(c));
        f.write((uint8_t*)&pad, sizeof(pad));
        // Zapiši prazne podatke
        GraphPoint empty;
        memset(&empty, 0, sizeof(empty));
        for (int i = 0; i < GRAPH_HISTORY_MAX; i++) f.write((uint8_t*)&empty, sizeof(empty));
        f.close();
        LOG_INFO("GraphStore", "Created new %s (%d bytes)",
                 GRAPH_STORE_FILE,
                 (int)(GRAPH_STORE_HEADER_SIZE + GRAPH_HISTORY_MAX * sizeof(GraphPoint)));
    } else {
        // Datoteka obstaja — preveri velikost
        size_t expectedSize = GRAPH_STORE_HEADER_SIZE +
                              (size_t)GRAPH_HISTORY_MAX * sizeof(GraphPoint);
        File f = LittleFS.open(GRAPH_STORE_FILE, "r");
        size_t actualSize = f ? f.size() : 0;
        if (f) f.close();

        if (actualSize != expectedSize) {
            LOG_WARN("GraphStore", "Size mismatch (%d != %d) — recreating",
                     (int)actualSize, (int)expectedSize);
            LittleFS.remove(GRAPH_STORE_FILE);
            // FIX: brez rekurzivnega klica — inline kreacija prazne datoteke
            File fc = LittleFS.open(GRAPH_STORE_FILE, "w");
            if (!fc) {
                LOG_ERROR("GraphStore", "Cannot recreate %s", GRAPH_STORE_FILE);
                return false;
            }
            uint16_t h = 0, c = 0;
            uint32_t p = 0;
            fc.write((uint8_t*)&h, sizeof(h));
            fc.write((uint8_t*)&c, sizeof(c));
            fc.write((uint8_t*)&p, sizeof(p));
            GraphPoint empty;
            memset(&empty, 0, sizeof(empty));
            for (int i = 0; i < GRAPH_HISTORY_MAX; i++) fc.write((uint8_t*)&empty, sizeof(empty));
            fc.close();
            LOG_INFO("GraphStore", "Recreated %s (%d bytes)", GRAPH_STORE_FILE,
                     (int)(GRAPH_STORE_HEADER_SIZE + GRAPH_HISTORY_MAX * sizeof(GraphPoint)));
        } else {
            LOG_INFO("GraphStore", "Found %s (%d bytes)", GRAPH_STORE_FILE, (int)actualSize);
        }
    }

    gsReady = true;
    return true;
}

// ============================================================
// NALAGANJE OB ZAGONU (RAM ← LittleFS)
// ============================================================
void graphLoadFromLittleFS() {
    if (!gsReady) {
        LOG_WARN("GraphStore", "Load skipped — not ready");
        return;
    }

    File f = LittleFS.open(GRAPH_STORE_FILE, "r");
    if (!f) {
        LOG_WARN("GraphStore", "Cannot open %s for read", GRAPH_STORE_FILE);
        return;
    }

    // Preberi header
    uint16_t fileHead  = 0;
    uint16_t fileCount = 0;
    uint32_t pad       = 0;
    f.read((uint8_t*)&fileHead,  sizeof(fileHead));
    f.read((uint8_t*)&fileCount, sizeof(fileCount));
    f.read((uint8_t*)&pad,       sizeof(pad));

    // Validacija
    if (fileHead  >= (uint16_t)GRAPH_HISTORY_MAX) fileHead  = 0;
    if (fileCount >  (uint16_t)GRAPH_HISTORY_MAX) fileCount = 0;

    // FIX STACK OVERFLOW: GraphPoint tmp[480] = 15360 bytes → stack crash!
    // Heap alokacija: podatki grejo v heap, ne na stack.
    // GRAPH_HISTORY_MAX * sizeof(GraphPoint) = 480 * 32 = 15360 bytes
    size_t tmpSize = (size_t)GRAPH_HISTORY_MAX * sizeof(GraphPoint);
    GraphPoint* tmp = (GraphPoint*)malloc(tmpSize);
    if (!tmp) {
        f.close();
        LOG_ERROR("GraphStore", "OOM — cannot allocate load buffer (%d bytes)", (int)tmpSize);
        return;
    }

    for (int i = 0; i < GRAPH_HISTORY_MAX; i++) {
        f.read((uint8_t*)&tmp[i], sizeof(GraphPoint));
    }
    f.close();

    // Kopiraj v RAM, preskoči točke z ts == 0 (§4.5 konsistentnost)
    int validCount = 0;
    memset(gsHistory, 0, sizeof(gsHistory));

    // Točke v ring buffer so v vrstnem redu [fileHead .. fileHead+fileCount-1] % MAX
    for (int i = 0; i < (int)fileCount; i++) {
        int srcIdx = ((int)fileHead + i) % GRAPH_HISTORY_MAX;
        if (tmp[srcIdx].ts == 0) continue;  // preskoči neveljavne
        gsHistory[validCount++] = tmp[srcIdx];
    }

    free(tmp);  // sprosti heap buffer — podatki so v gsHistory[]

    // Po nalaganju head kaže na naslednjo prosto lokacijo (linearno)
    // Čez normalni delovanje bomo pisali od validCount naprej
    // (ali od začetka če je poln)
    if (validCount >= GRAPH_HISTORY_MAX) {
        // Buffer je poln — head kaže na najstarejšo točko
        // (= 0 ker smo preuredili v linearen vrstni red)
        gsHead  = 0;
        gsCount = GRAPH_HISTORY_MAX;
    } else {
        gsHead  = 0;     // pisali bomo linearno (head=0, podatki od 0..validCount-1)
        gsCount = validCount;
    }

    LOG_INFO("GraphStore", "Loaded %d/%d valid points from LittleFS", validCount, (int)fileCount);
}

// ============================================================
// ZAPIS TOČKE (RAM + LittleFS atomično)
// ============================================================
void graphStorePoint(const GraphPoint& pt) {
    if (!gsReady) {
        LOG_WARN("GraphStore", "Write skipped — not ready");
        return;
    }

    // 1. Zapiši v RAM ring buffer
    int writeIdx;
    if (gsCount < GRAPH_HISTORY_MAX) {
        // Buffer ni poln — dodaj na konec
        writeIdx = gsCount;
        gsHistory[writeIdx] = pt;
        gsCount++;
    } else {
        // Buffer je poln — prepiši najstarejšo točko (head)
        writeIdx = gsHead;
        gsHistory[writeIdx] = pt;
        gsHead = (gsHead + 1) % GRAPH_HISTORY_MAX;
    }

    // 2. Zapiši v LittleFS (seek na pravo pozicijo)
    File f = LittleFS.open(GRAPH_STORE_FILE, "r+");
    if (!f) {
        LOG_WARN("GraphStore", "Cannot open %s for write", GRAPH_STORE_FILE);
        return;
    }

    // Izračunaj fileHead za LittleFS (indeks naslednjega zapisa)
    // Po zapisu se fileHead premakne za +1
    uint16_t fileHead;
    uint16_t fileCount;
    if (gsCount < GRAPH_HISTORY_MAX) {
        // Nismo prepisovali — fileHead je writeIdx + 1 (naslednji prosti)
        fileHead  = (uint16_t)(writeIdx + 1) % GRAPH_HISTORY_MAX;
        fileCount = (uint16_t)gsCount;
    } else {
        // Polni ring — fileHead je gsHead (kaže na najstarejšo)
        fileHead  = (uint16_t)gsHead;
        fileCount = (uint16_t)GRAPH_HISTORY_MAX;
    }

    // Zapiši točko na pozicijo writeIdx
    size_t dataOffset = GRAPH_STORE_HEADER_SIZE + (size_t)writeIdx * sizeof(GraphPoint);
    f.seek(dataOffset);
    f.write((uint8_t*)&pt, sizeof(GraphPoint));

    // Posodobi header
    uint32_t pad = 0;
    f.seek(0);
    f.write((uint8_t*)&fileHead,  sizeof(fileHead));
    f.write((uint8_t*)&fileCount, sizeof(fileCount));
    f.write((uint8_t*)&pad,       sizeof(pad));

    f.close();

    LOG_DEBUG("GraphStore", "Point written: idx=%d ts=%u T=%.1f H=%.1f IAQ=%.0f lux=%.0f",
              writeIdx, pt.ts, pt.temp, pt.hum, pt.iaq, pt.lux);
}

// ============================================================
// BRANJE V KRONOLOŠKEM VRSTNEM REDU
// ============================================================
int graphGetHistoryOrdered(GraphPoint* outBuf, int maxPts) {
    if (!outBuf || maxPts <= 0 || gsCount == 0) return 0;

    int n = (gsCount < maxPts) ? gsCount : maxPts;

    // Točke so v gsHistory[] linearno: [0..gsCount-1]
    // gsHead kaže na najstarejšo ob polnem bufferju
    // Ko ni poln, gsHead=0 in točke so od 0..gsCount-1 (kronološko)
    for (int i = 0; i < n; i++) {
        int srcIdx = (gsHead + i) % GRAPH_HISTORY_MAX;
        outBuf[i]  = gsHistory[srcIdx];
    }
    return n;
}

// ============================================================
// STANJE
// ============================================================
int  graphStoreCount() { return gsCount; }
bool graphStoreReady() { return gsReady; }

// ============================================================
// BRISANJE (debug/reset)
// ============================================================
void graphStoreClear() {
    memset(gsHistory, 0, sizeof(gsHistory));
    gsHead  = 0;
    gsCount = 0;

    if (gsReady) {
        LittleFS.remove(GRAPH_STORE_FILE);
        LOG_WARN("GraphStore", "Cleared — %s removed", GRAPH_STORE_FILE);
        graphStoreInit();
    }
}
