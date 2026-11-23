#pragma once
#include <stddef.h>
#include <stdint.h>

bool hwAudioInit();
void hwBeep(uint16_t freqHz, uint16_t durMs);
void hwTone(uint16_t f0Hz, uint16_t f1Hz, uint16_t durMs, uint8_t volPct = 100);
void hwPlayAdpcmFile(const char* path, uint8_t volPct = 100);
void hwPlayAdpcmOwned(uint8_t* data, size_t len, uint8_t volPct = 100);
uint32_t hwAudioQueueDropCount();   // 音频队列满导致的丢弃次数
uint32_t hwAudioFsFailCount();      // LittleFS 打开音频文件失败次数
