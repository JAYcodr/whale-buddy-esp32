#pragma once
#include <stddef.h>
#include <stdint.h>

struct HwRecSnapshot {
  bool     recording;
  bool     hasClip;
  bool     clipReady;        // true once a recording finishes until consumed
  uint16_t clipId;
  uint8_t  gainDb;
  uint8_t  liveLevel;        // 0..100
  uint32_t durMs;
  float    peakDb;
  float    rmsDb;
  uint16_t clipPermille;
  const int16_t* pcmData;    // PSRAM buffer, valid until next hwAudioRecStart()
  size_t   sampleCount;
};

bool hwAudioInit();
void hwBeep(uint16_t freqHz, uint16_t durMs);
void hwTone(uint16_t f0Hz, uint16_t f1Hz, uint16_t durMs, uint8_t volPct = 100);
void hwPlayAdpcmFile(const char* path, uint8_t volPct = 100);
void hwPlayAdpcmOwned(uint8_t* data, size_t len, uint8_t volPct = 100);
uint32_t hwAudioQueueDropCount();   // 音频队列满导致的丢弃次数
uint32_t hwAudioFsFailCount();      // LittleFS 打开音频文件失败次数

bool hwAudioRecStart(uint8_t gainDb);
void hwAudioRecStop();
void hwAudioRecCancel();
void hwAudioRecConsumeReady();
bool hwAudioRecPlayLast(uint8_t volPct);
HwRecSnapshot hwAudioRecSnapshot();
bool hwAudioApplyMicGainOnBus(uint8_t gainDb);  // ONLY call from task_bus (I2C owner)
