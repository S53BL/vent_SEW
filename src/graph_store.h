// graph_store.h - LittleFS ring buffer za zgodovino grafov (graph_update.md §4)
//
// Shrani do GRAPH_HISTORY_MAX GraphPoint zapisov (24h pri 3-min intervalu)
// v /graph.bin na LittleFS.
//
// Ključno načelo (§3.1 spec):
//   RAM in LittleFS sta vedno identični kopiji.
//   graphStorePoint() zapiše v oba sinhrono.
//   graphRefresh() bere SAMO iz RAM — nikoli direktno iz LittleFS med delovanjem.
//   Ob zagonu graphLoadFromLittleFS() obnovi RAM iz LittleFS.
//
// Struktura datoteke /graph.bin (§4.1):
//   [0..1]   head    uint16_t  indeks naslednjega zapisa (0..GRAPH_HISTORY_MAX-1)
//   [2..3]   count   uint16_t  število veljavnih zapisov (0..GRAPH_HISTORY_MAX)
//   [4..7]   rezerva uint32_t  za prihodnje uporabe (vpisano 0)
//   [8+]     data    GraphPoint[GRAPH_HISTORY_MAX]  ring buffer
//
// Skupaj: 8 + 480*32 = 15368 bytes (~15.0 KB)
// LittleFS razpoložljivo: ~2.9 MB → poraba 0.53%
//
// Konsistentnost ob izpadu napajanja (§4.5):
//   graphLoadFromLittleFS() preveri ts vsake točke: ts > 0.
//   Neveljaven ts → točka se preskoči.
//
// OPOMBA: LittleFS je na ločenem flash-u, ne deli SPI busa z LCD/SD.
//   → sdMutex pri LittleFS ni potreben. SD operacije ga še vedno zahtevajo.

#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include "disp_graph.h"   // GraphPoint, GRAPH_HISTORY_MAX

// -----------------------------------------------------------------------
// Konstante
// -----------------------------------------------------------------------
#define GRAPH_STORE_FILE         "/graph.bin"
#define GRAPH_STORE_HEADER_SIZE  8    // head(2) + count(2) + rezerva(4)

// -----------------------------------------------------------------------
// API (graph_update.md §4.2)
// -----------------------------------------------------------------------

// Inicializacija: odpre LittleFS, ustvari/validira /graph.bin.
// Kliči enkrat v setup() — PRED graphLoadFromLittleFS().
// Vrne true ob uspehu.
bool graphStoreInit();

// Ob zagonu: naloži /graph.bin → RAM history[].
// Kliči po graphStoreInit() in pred initGraph() v setup().
void graphLoadFromLittleFS();

// Zapiši točko v RAM history[] IN v /graph.bin sinhrono.
// Kliči enkrat vsake 3 minute iz 3-minutne zanke.
void graphStorePoint(const GraphPoint& pt);

// Vrne točke v kronološkem vrstnem redu (najstarejša→najnovejša) v outBuf[].
// maxPts: največje število točk za kopiranje (tipično DISPLAY_POINTS ali GRAPH_HISTORY_MAX).
// Vrne dejansko število kopiranih točk.
int graphGetHistoryOrdered(GraphPoint* outBuf, int maxPts);

// Vrne število veljavnih točk v RAM bufferju (0..GRAPH_HISTORY_MAX).
int graphStoreCount();

// Vrne true če graphStoreInit() je bil uspešen in je LittleFS dosegljiv.
bool graphStoreReady();

// Izbriše /graph.bin in ponastavi RAM buffer (za debug/reset).
void graphStoreClear();
