// sd.h - SD card module header for vent_SEW
//
// Knjižnica: SD (SPI način) - ker SD deli SPI bus z LCD (IO38/IO39)
// Pini: MOSI=IO38, SCLK=IO39, MISO=IO40, CS=IO41 (iz config.h)
//
// Datoteke na SD:
//   sew_YYYY-MM-DD.csv   - senzorske meritve (vsak DATA_SEND_INTERVAL)
//   log_YYYY-MM-DD.txt   - sistemski logi (logging.cpp)

#ifndef SD_SEW_H
#define SD_SEW_H

#include <SD.h>
#include <stdint.h>

// Inicializacija SD kartice (SPI način, z mutexom)
bool initSD();

// Shrani senzorske meritve ob vsakem pošiljanju na REW
// Ustvari sew_YYYY-MM-DD.csv če ne obstaja
void saveSDData();

// Preberi datoteko z SD (za web handler /sd-file)
String readFileSD(const char* path);

// Vrne JSON seznam datotek v root direktoriju (za web vmesnik)
String listFilesSD();

#endif // SD_SEW_H
