// sens.h - Sensor module header for vent_SEW
//
// Senzorji:
//   SHT41    (0x44) - temperatura, vlažnost          → I2C bus 0 (Wire1, IO48/IO47)
//   BME680   (0x76) - tlak, IAQ, eCO2, breathVOC     → I2C bus 0 (Wire1, IO48/IO47)
//   TCS34725 (0x29) - svetloba (lux), CCT, RGB       → I2C bus 0 (Wire1, IO48/IO47)
//   PIR      (IO18) - gibanje (digitalni vhod) - P1:6
//
// I2C bus recovery:
//   - checkI2CDevice() pred vsakim branjem
//   - resetI2CBus() ob napaki (9x SCL toggle + Wire1.end/begin)
//   - error counter: senzor se označi kot absent po N zaporednih napakah
//   - performPeriodicSensorCheck(): reconnect vsakih 10 min
//   - performPeriodicI2CReset(): preventivni reset vsakih 30 min
//
// newSensorData flag:
//   - readSensors() postavi na true po uspešnem branju SHT41
//   - main.cpp prebere in postavi nazaj na false po graphAddPoint()
//
// runBsecLoop():
//   - Klic iz main loop() pri vsakem prehodu
//   - BSEC2 SAMPLE_RATE_LP zahteva run() vsaj vsake 3s za sproži callback
//   - run() je neblokirajoča: če ni novih podatkov, se takoj vrne
//
// Baterija:
//   - ADC IO5, delilnik 200K/100K → faktor ×3
//   - readBattery() bere in posodobi sensorData.bat + sensorData.batPct

#ifndef SENS_H
#define SENS_H

#include "config.h"

// Inicializacija
bool initSensors();
bool initSens();    // Alias za main.cpp

// Branje (klic iz main loop)
void readSensors();
void runSens();     // Alias za main.cpp (wrapper → kliče readSHT41+readTCS+readPIR)

// Ločene funkcije za 3-consko arhitekturo (graph_update.md):
void readSHT41();   // Samo SHT41: temp + hum (3-min glavna zanka)
void readTCS();     // Samo TCS34725: lux + cct + rgb (3-min glavna zanka)
void readPIR();     // Samo PIR: edge detection, motionCount++ (1s hitra zanka)
void runBsec();     // Samo BSEC2: iaqSensor.run() → callback (1s hitra zanka)
void runBsecLoop(); // Alias za runBsec() — ohranjen za kompatibilnost

// I2C bus recovery
void initI2CBus();
bool checkI2CDevice(uint8_t address);
void resetI2CBus();

// Periodični checks (klic iz main loop)
void performPeriodicSensorCheck();  // reconnect, vsakih 10 min
void performPeriodicI2CReset();     // preventivni reset, vsakih 30 min

// Baterija
void readBattery();

// PIR motion event — zapis na SD (klic iz readPIR() ob FALLING EDGE)
void logMotionEvent(time_t ts);

// Reset (ob kritični napaki)
void resetSensors();

#endif // SENS_H
