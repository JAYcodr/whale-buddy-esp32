#pragma once
#include <stddef.h>
#include <stdint.h>

struct HwSdInfo {
  bool     mounted;
  bool     rawCardOk;
  uint8_t  cardType;      // 0=NONE, 1=MMC, 2=SDSC, 3=SDHC/SDXC, 4=UNKNOWN
  uint32_t freqKhz;
  int32_t  lastErr;       // esp_err_t from probe/mount when failed
  uint64_t cardSizeBytes;
  uint64_t totalBytes;
  uint64_t usedBytes;
  uint32_t sectorSize;
  char     cardName[8];
};

// Initialize and mount TF card if present (never fatal; returns mounted status).
bool hwSdInit();
bool hwSdMounted();
HwSdInfo hwSdInfo();

// Serial diagnostics: list root directory, run non-destructive 64 KiB R/W verification,
// or explicitly format an unformatted/corrupted card to FAT32 (destructive!).
void hwSdPrintStatus(bool listRoot);
void hwSdRunSelfTest();
bool hwSdFormat();

// Host-side TF asset staging & read helpers (paths relative to /sdcard mount, e.g. "/pets/clawd/idle.nva").
bool hwSdMkdir(const char* relDir);
bool hwSdWriteFileChunk(const char* relPath, bool append, const uint8_t* data, size_t len);
bool hwSdExists(const char* relPath);
size_t hwSdFileSize(const char* relPath);
size_t hwSdReadFile(const char* relPath, uint8_t* dst, size_t dstLen);
