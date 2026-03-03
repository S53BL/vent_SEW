// graph_store.h - LittleFS ring buffer za zgodovino grafov (vent_SEW)
//
// Shrani do 480 GraphStorePoint zapisov (24h pri 3-min intervalu) v /graph.bin.
// Ring buffer deluje brez premikanja podatkov — samo head kazalec se premika.
// Ob restartu graphStoreLoad() obnovi RAM buffer iz LittleFS.
//
// Struktura datoteke /graph.bin:
//   [0..3]   magic   uint32_t  0xC0FFEE01
//   [4..7]   head    uint32_t  indeks naslednjega zapisa (0..479)
//   [8..11]  count   uint32_t  stevilo veljavnih zapisov (0..480)
//   [12+]    data    GraphStorePoint[480]
//
// Skupaj: 12 + 480*24 = 11532 bytes (~11.3 KB)

#pragma once
#include <Arduino.h>
#include <LittleFS.h>

// -----------------------------------------------------------------------
// GraphStorePoint — ena meritev za shranjevanje (24 bytes, poravnana)
// -----------------------------------------------------------------------
struct GraphStorePoint {
    uint32_t ts;        // Unix timestamp [s]           (4 bytes)
    float    temp;      // Temperatura [°C]             (4 bytes)
    float    hum;       // Relativna vlažnost [%]       (4 bytes)
    float    iaq;       // IAQ indeks 0–500             (4 bytes)
    float    wind;      // Hitrost vetra [km/h]         (4 bytes)
    uint16_t motion;    // Število PIR dogodkov v uri   (2 bytes)
    uint16_t pad;       // Poravnava na 24 bytes        (2 bytes)
};
static_assert(sizeof(GraphStorePoint) == 24, "GraphStorePoint mora biti 24 bytes");

// -----------------------------------------------------------------------
// Konstante
// -----------------------------------------------------------------------
#define GRAPH_STORE_MAX_POINTS   480          // 24h pri 3-min intervalu
#define GRAPH_STORE_FILE         "/graph.bin"
#define GRAPH_STORE_MAGIC        0xC0FFEE01u

// Velikost glave datoteke
#define GRAPH_STORE_HEADER_SIZE  12  // magic(4) + head(4) + count(4)

// Offset prvega zapisa v datoteki
#define GRAPH_STORE_DATA_OFFSET  GRAPH_STORE_HEADER_SIZE

// -----------------------------------------------------------------------
// RAM buffer (extern — dostopen za graphRefresh v disp_graph.cpp)
// -----------------------------------------------------------------------
extern GraphStorePoint gsHistory[GRAPH_STORE_MAX_POINTS];
extern int        gsHead;    // indeks najstarejšega veljavnega zapisa
extern int        gsCount;   // število veljavnih zapisov (0..480)

// -----------------------------------------------------------------------
// API
// -----------------------------------------------------------------------

// Inicializacija: odpre LittleFS (če ni že odprt), ustvari/validira /graph.bin
// Kliči enkrat v setup() — PRED graphStoreLoad()
// Vrne true ob uspehu.
bool graphStoreInit();

// Naloži obstoječe točke iz /graph.bin v RAM buffer (gsHistory, gsHead, gsCount)
// Kliči po graphStoreInit() v setup()
void graphStoreLoad();

// Doda novo točko v ring buffer (RAM + LittleFS atomično)
// Kliči enkrat vsake 3 minute iz glavne zanke
void graphStoreAdd(const GraphStorePoint& pt);

// Vrne kazalec na točko po logičnem indeksu (0 = najstarejša, gsCount-1 = najnovejša)
// Vrne nullptr če idx izven dosega
const GraphStorePoint* graphStoreGet(int idx);

// Izbriše /graph.bin in ponastavi RAM buffer (za debug/reset)
void graphStoreClear();
