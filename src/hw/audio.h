#pragma once
#include <stdint.h>

bool hwAudioInit();
void hwBeep(uint16_t freqHz, uint16_t durMs);
void hwTone(uint16_t f0Hz, uint16_t f1Hz, uint16_t durMs, uint8_t volPct = 100);
