// example_wifi_config.h - PRIMER konfiguracije WiFi za vent_SEW
//
// POZOR: Ta datoteka je PRIMER za Git repozitorij.
// Za dejansko uporabo:
//   1. Kopiraj to datoteko kot wifi_config.h
//   2. Vstavi dejanske SSID in gesla
//   3. wifi_config.h je v .gitignore in se NE bo naložila v Git
//
// Format:
//   ssidList[]     = lista omrežij (preizkuša po vrsti)
//   passwordList[] = gesla (isti indeks kot ssidList)
//   numNetworks    = število omrežij

#pragma once

const char* ssidList[] = {
    "MojeOmrezje",        // primarno
    "GostOmrezje",        // rezervno
};

const char* passwordList[] = {
    "mojegeslo123",
    "gostgeslo456",
};

const int numNetworks = 2;
