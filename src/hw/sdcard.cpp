#include "hw/sdcard.h"
#include "hw/pins.h"
#include <Arduino.h>
#include <string.h>

#if BOARD_HAS_SDMMC
#include <FS.h>
#include <SD_MMC.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"

namespace {

HwSdInfo s_info = {};

const char* cardTypeName(uint8_t t) {
  switch (t) {
    case CARD_MMC:     return "MMC";
    case CARD_SD:      return "SDSC";
    case CARD_SDHC:    return "SDHC/SDXC";
    case CARD_UNKNOWN: return "UNKNOWN";
    default:           return "NONE";
  }
}

void printHexLine(const char* tag, const uint8_t* p, size_t off, size_t len) {
  Serial.printf("%s+%03u:", tag, (unsigned)off);
  for (size_t i = 0; i < len; i++) {
    Serial.printf(" %02X", p[off + i]);
  }
  Serial.print(" |");
  for (size_t i = 0; i < len; i++) {
    uint8_t c = p[off + i];
    Serial.print((c >= 0x20 && c <= 0x7E) ? (char)c : '.');
  }
  Serial.println("|");
}

void inspectRawSectors(sdmmc_card_t* card) {
  uint8_t* sec = (uint8_t*)heap_caps_malloc(512, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  uint8_t* secB = (uint8_t*)heap_caps_malloc(512, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!sec || !secB) { free(sec); free(secB); return; }
  esp_err_t r0 = sdmmc_read_sectors(card, sec, 0, 1);
  esp_err_t r1 = sdmmc_read_sectors(card, secB, 0, 1);
  if (r0 != ESP_OK || r1 != ESP_OK) {
    Serial.printf("SD_RAW sec0 read err=0x%x/0x%x (%s)\n", (unsigned)r0, (unsigned)r1, esp_err_to_name(r0));
    free(sec); free(secB);
    return;
  }
  bool stable = (memcmp(sec, secB, 512) == 0);
  bool allZero = true;
  for (int i = 0; i < 512; i++) {
    if (sec[i] != 0) { allZero = false; break; }
  }
  Serial.printf("SD_RAW sec0 stable=%d zero=%d sig=%02X%02X\n",
                stable ? 1 : 0, allZero ? 1 : 0, sec[510], sec[511]);
  for (size_t off = 0; off < 64; off += 16) printHexLine("SD0 ", sec, off, 16);
  for (size_t off = 448; off < 512; off += 16) printHexLine("SD0 ", sec, off, 16);

  uint32_t part1Start = 0;
  for (int p = 0; p < 4; p++) {
    const uint8_t* e = sec + 446 + p * 16;
    uint8_t ptype = e[4];
    uint32_t lba = (uint32_t)e[8] | ((uint32_t)e[9] << 8) | ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
    uint32_t cnt = (uint32_t)e[12] | ((uint32_t)e[13] << 8) | ((uint32_t)e[14] << 16) | ((uint32_t)e[15] << 24);
    if (ptype != 0 || lba != 0 || cnt != 0) {
      Serial.printf("SD_RAW part%d type=0x%02X lba=%lu sectors=%lu (%luMB)\n",
                    p + 1, ptype, (unsigned long)lba, (unsigned long)cnt,
                    (unsigned long)((uint64_t)cnt * 512ULL / (1024ULL * 1024ULL)));
      if (p == 0) part1Start = lba;
    }
  }
  if (part1Start > 0 && part1Start < (uint32_t)card->csd.capacity &&
      sdmmc_read_sectors(card, sec, part1Start, 1) == ESP_OK) {
    printHexLine("SDP1", sec, 0, 16);
  }
  const uint32_t probes[] = {1, 63, 128, 2048, 8192, 32768,
                             card->csd.capacity > 0 ? (uint32_t)(card->csd.capacity - 1) : 0};
  for (uint32_t lba : probes) {
    if (lba == 0 || lba >= (uint32_t)card->csd.capacity) continue;
    esp_err_t pr = sdmmc_read_sectors(card, sec, lba, 1);
    if (pr != ESP_OK) {
      Serial.printf("SD_LBA %lu err=0x%x (%s)\n", (unsigned long)lba, (unsigned)pr, esp_err_to_name(pr));
      continue;
    }
    bool z = true, ff = true;
    for (int i = 0; i < 512; i++) {
      if (sec[i] != 0x00) z = false;
      if (sec[i] != 0xFF) ff = false;
    }
    Serial.printf("SD_LBA %-8lu z=%d ff=%d sig=%02X%02X |",
                  (unsigned long)lba, z ? 1 : 0, ff ? 1 : 0, sec[510], sec[511]);
    for (int i = 0; i < 16; i++) Serial.printf(" %02X", sec[i]);
    Serial.print(" |");
    for (int i = 0; i < 16; i++) {
      uint8_t c = sec[i];
      Serial.print((c >= 0x20 && c <= 0x7E) ? (char)c : '.');
    }
    Serial.println("|");
  }
  free(sec);
  free(secB);
}

void probeAtFreq(uint32_t freqKhz, const uint8_t* refSec0) {
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.flags = SDMMC_HOST_FLAG_1BIT;
  host.slot = SDMMC_HOST_SLOT_1;
  host.max_freq_khz = (int)freqKhz;

  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.clk = (gpio_num_t)PIN_SD_CLK;
  slot.cmd = (gpio_num_t)PIN_SD_CMD;
  slot.d0  = (gpio_num_t)PIN_SD_D0;
  slot.width = 1;
  slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  sdmmc_card_t card = {};
  esp_err_t err = host.init();
  if (err != ESP_OK) {
    Serial.printf("SD_PROBE %lukHz host.init err=0x%x\n", (unsigned long)freqKhz, (unsigned)err);
    return;
  }
  err = sdmmc_host_init_slot(host.slot, &slot);
  if (err == ESP_OK) err = sdmmc_card_init(&host, &card);
  if (err != ESP_OK) {
    Serial.printf("SD_PROBE %lukHz card_init err=0x%x (%s)\n", (unsigned long)freqKhz, (unsigned)err, esp_err_to_name(err));
    host.deinit();
    return;
  }
  uint8_t* blk = (uint8_t*)heap_caps_malloc(2048, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (blk) {
    esp_err_t mr = sdmmc_read_sectors(&card, blk, 0, 4);
    bool match0 = (mr == ESP_OK && refSec0 && memcmp(blk, refSec0, 512) == 0);
    Serial.printf("SD_PROBE %lukHz real=%dkHz multi4_err=0x%x (%s) match_sec0=%d\n",
                  (unsigned long)freqKhz, card.real_freq_khz, (unsigned)mr, esp_err_to_name(mr), match0 ? 1 : 0);
    free(blk);
  }
  host.deinit();
}

void probeRawCard(bool verbose) {
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.flags = SDMMC_HOST_FLAG_1BIT;
  host.slot = SDMMC_HOST_SLOT_1;
  host.max_freq_khz = SDMMC_FREQ_PROBING;
  if (!verbose) {
    // Keep empty-slot / hung-card boot probing fast (<0.8s instead of ~3.3s x 3).
    host.command_timeout_ms = 250;
  }

  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.clk = (gpio_num_t)PIN_SD_CLK;
  slot.cmd = (gpio_num_t)PIN_SD_CMD;
  slot.d0  = (gpio_num_t)PIN_SD_D0;
  slot.width = 1;
  slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  sdmmc_card_t card = {};
  esp_err_t err = host.init();
  if (err != ESP_OK) {
    s_info.lastErr = (int32_t)err;
    return;
  }
  err = sdmmc_host_init_slot(host.slot, &slot);
  if (err == ESP_OK) {
    err = sdmmc_card_init(&host, &card);
    if (err == ESP_OK) {
      s_info.rawCardOk = true;
      s_info.cardSizeBytes = (uint64_t)card.csd.capacity * (uint64_t)card.csd.sector_size;
      s_info.sectorSize = (uint32_t)card.csd.sector_size;
      s_info.cardType = (card.ocr & (1u << 30)) ? CARD_SDHC : CARD_SD;
      memcpy(s_info.cardName, card.cid.name, 5);
      s_info.cardName[5] = '\0';
      s_info.lastErr = (int32_t)ESP_FAIL;  // Card responded at protocol level; VFS FAT mount pending/failed
      if (verbose) {
        Serial.printf("SD_RAW ocr=0x%08lX rca=0x%04X mem=%u mmc=%u csd_v=%d cap=%d freq=%d\n",
                      (unsigned long)card.ocr, card.rca, card.is_mem, card.is_mmc,
                      card.csd.csd_ver, card.csd.capacity, card.real_freq_khz);
        Serial.printf("SD_CID mfg=0x%02X oem=0x%04X name='%c%c%c%c%c' rev=%d date=%d ser=0x%08X\n",
                      card.cid.mfg_id, card.cid.oem_id,
                      (card.cid.name[0] >= 0x20 && card.cid.name[0] <= 0x7E) ? card.cid.name[0] : '.',
                      (card.cid.name[1] >= 0x20 && card.cid.name[1] <= 0x7E) ? card.cid.name[1] : '.',
                      (card.cid.name[2] >= 0x20 && card.cid.name[2] <= 0x7E) ? card.cid.name[2] : '.',
                      (card.cid.name[3] >= 0x20 && card.cid.name[3] <= 0x7E) ? card.cid.name[3] : '.',
                      (card.cid.name[4] >= 0x20 && card.cid.name[4] <= 0x7E) ? card.cid.name[4] : '.',
                      card.cid.revision, card.cid.date, (unsigned)card.cid.serial);
        sdmmc_card_print_info(stdout, &card);
        inspectRawSectors(&card);
        uint8_t* ref0 = (uint8_t*)heap_caps_malloc(512, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (ref0 && sdmmc_read_sectors(&card, ref0, 0, 1) != ESP_OK) {
          free(ref0);
          ref0 = nullptr;
        }
        host.deinit();
        probeAtFreq(SDMMC_FREQ_DEFAULT, ref0);
        free(ref0);
        return;
      }
      host.deinit();
      return;
    }
  }
  s_info.lastErr = (int32_t)err;
  host.deinit();
}

bool tryMountAtFreq(uint32_t freqKhz) {
  SD_MMC.end();
  if (!SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0)) {
    return false;
  }
  if (!SD_MMC.begin("/sdcard", true, false, (int)freqKhz, 5)) {
    return false;
  }
  uint8_t ctype = SD_MMC.cardType();
  if (ctype == CARD_NONE) {
    SD_MMC.end();
    return false;
  }
  s_info.mounted       = true;
  s_info.rawCardOk     = true;
  s_info.cardType      = ctype;
  s_info.freqKhz       = freqKhz;
  s_info.lastErr       = ESP_OK;
  s_info.cardSizeBytes = SD_MMC.cardSize();
  s_info.totalBytes    = SD_MMC.totalBytes();
  s_info.usedBytes     = SD_MMC.usedBytes();
  s_info.sectorSize    = (uint32_t)SD_MMC.sectorSize();
  return true;
}

}  // namespace

bool hwSdInit() {
  memset(&s_info, 0, sizeof(s_info));
  // Fast probe first: skip slow 20/10 MHz FAT retries if slot is empty or card fails OCR/CMD init.
  probeRawCard(false);
  if (s_info.rawCardOk && (tryMountAtFreq(SDMMC_FREQ_DEFAULT) || tryMountAtFreq(10000))) {
    Serial.printf("hwSdInit OK: %s card=%lluMB fs=%llu/%lluMB @%luKHz\n",
                  cardTypeName(s_info.cardType),
                  (unsigned long long)(s_info.cardSizeBytes / (1024ULL * 1024ULL)),
                  (unsigned long long)(s_info.usedBytes / (1024ULL * 1024ULL)),
                  (unsigned long long)(s_info.totalBytes / (1024ULL * 1024ULL)),
                  (unsigned long)s_info.freqKhz);
    return true;
  }
  Serial.printf("hwSdInit FAIL: rawOk=%d type=%s card=%lluMB name='%s' err=0x%lx (%s)\n",
                s_info.rawCardOk ? 1 : 0,
                cardTypeName(s_info.cardType),
                (unsigned long long)(s_info.cardSizeBytes / (1024ULL * 1024ULL)),
                s_info.cardName,
                (unsigned long)s_info.lastErr,
                esp_err_to_name((esp_err_t)s_info.lastErr));
  return false;
}

bool hwSdMounted() { return s_info.mounted; }
HwSdInfo hwSdInfo() { return s_info; }

void hwSdPrintStatus(bool listRoot) {
  if (s_info.mounted) {
    s_info.totalBytes = SD_MMC.totalBytes();
    s_info.usedBytes  = SD_MMC.usedBytes();
  }
  Serial.printf("{\"sd\":{\"mounted\":%s,\"rawOk\":%s,\"type\":\"%s\",\"name\":\"%s\","
                "\"freqKhz\":%lu,\"sectorSize\":%lu,\"cardMB\":%llu,\"totalMB\":%llu,"
                "\"usedMB\":%llu,\"err\":\"0x%lx %s\"}}\n",
                s_info.mounted ? "true" : "false",
                s_info.rawCardOk ? "true" : "false",
                cardTypeName(s_info.cardType),
                s_info.cardName,
                (unsigned long)s_info.freqKhz,
                (unsigned long)s_info.sectorSize,
                (unsigned long long)(s_info.cardSizeBytes / (1024ULL * 1024ULL)),
                (unsigned long long)(s_info.totalBytes / (1024ULL * 1024ULL)),
                (unsigned long long)(s_info.usedBytes / (1024ULL * 1024ULL)),
                (unsigned long)s_info.lastErr,
                esp_err_to_name((esp_err_t)s_info.lastErr));

  if (!listRoot || !s_info.mounted) return;
  File root = SD_MMC.open("/");
  if (!root || !root.isDirectory()) {
    Serial.println("SD_LS: failed to open /");
    if (root) root.close();
    return;
  }
  int count = 0;
  File entry = root.openNextFile();
  while (entry && count < 32) {
    if (entry.isDirectory()) {
      Serial.printf("SD_DIR  %s\n", entry.name());
    } else {
      Serial.printf("SD_FILE %s (%lu bytes)\n", entry.name(), (unsigned long)entry.size());
    }
    entry.close();
    count++;
    entry = root.openNextFile();
  }
  if (entry) {
    entry.close();
    Serial.println("SD_LS: (more entries omitted)");
  }
  root.close();
}

void hwSdRunSelfTest() {
  if (!s_info.mounted) {
    hwSdInit();
  }
  if (!s_info.mounted) {
    probeRawCard(true);
    hwSdPrintStatus(false);
    return;
  }

  constexpr size_t kChunk = 4096;
  constexpr size_t kTotal = 65536;
  static const char* kTestPath = "/_nova_sdtest.tmp";

  uint8_t* buf = (uint8_t*)malloc(kChunk);
  if (!buf) {
    Serial.println("{\"sdtest\":{\"ok\":false,\"err\":\"oom\"}}");
    return;
  }

  uint32_t t0 = millis();
  File wf = SD_MMC.open(kTestPath, FILE_WRITE);
  if (!wf) {
    free(buf);
    Serial.println("{\"sdtest\":{\"ok\":false,\"err\":\"open_write\"}}");
    return;
  }
  bool writeOk = true;
  for (size_t off = 0; off < kTotal; off += kChunk) {
    for (size_t i = 0; i < kChunk; i++) {
      buf[i] = (uint8_t)(((off + i) * 131u + 17u) & 0xFFu);
    }
    if (wf.write(buf, kChunk) != kChunk) {
      writeOk = false;
      break;
    }
  }
  wf.flush();
  wf.close();
  uint32_t writeMs = millis() - t0;

  if (!writeOk) {
    SD_MMC.remove(kTestPath);
    free(buf);
    Serial.println("{\"sdtest\":{\"ok\":false,\"err\":\"write_short\"}}");
    return;
  }

  uint32_t t1 = millis();
  File rf = SD_MMC.open(kTestPath, FILE_READ);
  if (!rf) {
    SD_MMC.remove(kTestPath);
    free(buf);
    Serial.println("{\"sdtest\":{\"ok\":false,\"err\":\"open_read\"}}");
    return;
  }
  bool readOk = true;
  size_t mismatchAt = SIZE_MAX;
  size_t readBytes = 0;
  while (readBytes < kTotal) {
    size_t n = rf.read(buf, kChunk);
    if (n == 0) { readOk = false; break; }
    for (size_t i = 0; i < n; i++) {
      uint8_t exp = (uint8_t)(((readBytes + i) * 131u + 17u) & 0xFFu);
      if (buf[i] != exp) {
        mismatchAt = readBytes + i;
        readOk = false;
        break;
      }
    }
    if (!readOk) break;
    readBytes += n;
  }
  rf.close();
  uint32_t readMs = millis() - t1;

  bool rmOk = SD_MMC.remove(kTestPath);
  free(buf);

  uint32_t wKbps = writeMs > 0 ? (uint32_t)((kTotal / 1024u) * 1000u / writeMs) : 0;
  uint32_t rKbps = readMs  > 0 ? (uint32_t)((kTotal / 1024u) * 1000u / readMs)  : 0;
  Serial.printf("{\"sdtest\":{\"ok\":%s,\"bytes\":%u,\"writeMs\":%lu,\"writeKBps\":%lu,"
                "\"readMs\":%lu,\"readKBps\":%lu,\"mismatchAt\":%ld,\"removed\":%s}}\n",
                (readOk && readBytes == kTotal) ? "true" : "false",
                (unsigned)readBytes,
                (unsigned long)writeMs, (unsigned long)wKbps,
                (unsigned long)readMs,  (unsigned long)rKbps,
                (mismatchAt == SIZE_MAX) ? -1L : (long)mismatchAt,
                rmOk ? "true" : "false");
  hwSdPrintStatus(true);
}

struct SdFmtWorkerCtx {
  volatile bool done = false;
  bool ok = false;
  bool wipeErr = false;
  esp_err_t err = ESP_OK;
  uint32_t fmtMs = 0;
};

static void sdFmtWorkerTask(void* arg) {
  SdFmtWorkerCtx* ctx = (SdFmtWorkerCtx*)arg;
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.flags = SDMMC_HOST_FLAG_1BIT;
  host.slot = SDMMC_HOST_SLOT_1;
  host.max_freq_khz = SDMMC_FREQ_DEFAULT;

  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.clk = (gpio_num_t)PIN_SD_CLK;
  slot.cmd = (gpio_num_t)PIN_SD_CMD;
  slot.d0  = (gpio_num_t)PIN_SD_D0;
  slot.width = 1;
  slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  // Wipe sector 0 first so stale/corrupt MBR entries cannot confuse FatFS f_fdisk/f_mkfs.
  esp_err_t werr = ESP_OK;
  {
    sdmmc_host_t phost = host;
    phost.max_freq_khz = SDMMC_FREQ_PROBING;
    sdmmc_card_t pcard = {};
    werr = phost.init();
    if (werr == ESP_OK) werr = sdmmc_host_init_slot(phost.slot, &slot);
    if (werr == ESP_OK) werr = sdmmc_card_init(&phost, &pcard);
    if (werr == ESP_OK) {
      uint8_t* zero = (uint8_t*)heap_caps_calloc(1, 512, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
      if (zero) {
        werr = sdmmc_write_sectors(&pcard, zero, 0, 1);
        free(zero);
      } else {
        werr = ESP_ERR_NO_MEM;
      }
    }
    phost.deinit();
  }
  if (werr != ESP_OK) {
    ctx->wipeErr = true;
    ctx->err = werr;
    ctx->done = true;
    vTaskDelete(nullptr);
    return;
  }

  esp_vfs_fat_sdmmc_mount_config_t mcfg = {
    .format_if_mount_failed = true,
    .max_files = 5,
    .allocation_unit_size = 32 * 1024,
    .disk_status_check_enable = false,
    .use_one_fat = false,
  };
  sdmmc_card_t* mountedCard = nullptr;
  uint32_t t0 = millis();
  esp_err_t err = esp_vfs_fat_sdmmc_mount("/sdcard_fmt", &host, &slot, &mcfg, &mountedCard);
  ctx->fmtMs = millis() - t0;
  if (err != ESP_OK || !mountedCard) {
    ctx->err = err;
    ctx->done = true;
    vTaskDelete(nullptr);
    return;
  }
  esp_vfs_fat_sdcard_unmount("/sdcard_fmt", mountedCard);
  ctx->ok = true;
  ctx->done = true;
  vTaskDelete(nullptr);
}

static bool validRelSdPath(const char* p) {
  return p && p[0] == '/' && p[1] != '\0' && strstr(p, "..") == nullptr && strlen(p) < 120;
}

bool hwSdFormat() {
  SD_MMC.end();
  memset(&s_info, 0, sizeof(s_info));

  // Offload potentially long (~5-15s) flash erase/mkfs to a non-WDT task while yielding task_ui.
  SdFmtWorkerCtx* ctx = new SdFmtWorkerCtx();
  if (!ctx) return false;
  if (xTaskCreate(sdFmtWorkerTask, "sd_fmt", 6144, ctx, 2, nullptr) != pdPASS) {
    delete ctx;
    return false;
  }
  while (!ctx->done) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  if (ctx->wipeErr) {
    s_info.lastErr = (int32_t)ctx->err;
    Serial.printf("{\"sdformat\":{\"ok\":false,\"stage\":\"wipe_mbr\",\"err\":\"0x%lx %s\"}}\n",
                  (unsigned long)ctx->err, esp_err_to_name(ctx->err));
    delete ctx;
    return false;
  }
  if (!ctx->ok) {
    s_info.lastErr = (int32_t)ctx->err;
    Serial.printf("{\"sdformat\":{\"ok\":false,\"stage\":\"mkfs\",\"ms\":%lu,\"err\":\"0x%lx %s\"}}\n",
                  (unsigned long)ctx->fmtMs, (unsigned long)ctx->err, esp_err_to_name(ctx->err));
    delete ctx;
    return false;
  }
  uint32_t fmtMs = ctx->fmtMs;
  delete ctx;

  bool ok = hwSdInit();
  Serial.printf("{\"sdformat\":{\"ok\":%s,\"ms\":%lu,\"cardMB\":%llu,\"totalMB\":%llu}}\n",
                ok ? "true" : "false",
                (unsigned long)fmtMs,
                (unsigned long long)(s_info.cardSizeBytes / (1024ULL * 1024ULL)),
                (unsigned long long)(s_info.totalBytes / (1024ULL * 1024ULL)));
  if (ok) {
    hwSdRunSelfTest();
  }
  return ok;
}

bool hwSdMkdir(const char* relDir) {
  if (!s_info.mounted || !validRelSdPath(relDir)) return false;
  char tmp[128];
  snprintf(tmp, sizeof(tmp), "%s", relDir);
  for (char* q = tmp + 1; *q; q++) {
    if (*q == '/') {
      *q = '\0';
      if (!SD_MMC.exists(tmp) && !SD_MMC.mkdir(tmp)) return false;
      *q = '/';
    }
  }
  return SD_MMC.exists(tmp) ? true : SD_MMC.mkdir(tmp);
}

bool hwSdWriteFileChunk(const char* relPath, bool append, const uint8_t* data, size_t len) {
  if (!s_info.mounted || !validRelSdPath(relPath) || (!data && len > 0)) return false;
  if (!append) {
    char parent[128];
    snprintf(parent, sizeof(parent), "%s", relPath);
    char* slash = strrchr(parent, '/');
    if (slash && slash > parent) {
      *slash = '\0';
      if (!hwSdMkdir(parent)) return false;
    }
  }
  File f = SD_MMC.open(relPath, append ? FILE_APPEND : FILE_WRITE);
  if (!f) return false;
  size_t written = (len > 0) ? f.write(data, len) : 0;
  f.flush();
  f.close();
  return written == len;
}

bool hwSdExists(const char* relPath) {
  return s_info.mounted && validRelSdPath(relPath) && SD_MMC.exists(relPath);
}

size_t hwSdFileSize(const char* relPath) {
  if (!s_info.mounted || !validRelSdPath(relPath)) return 0;
  File f = SD_MMC.open(relPath, "r");
  if (!f) return 0;
  size_t sz = f.size();
  f.close();
  return sz;
}

size_t hwSdReadFile(const char* relPath, uint8_t* dst, size_t dstLen) {
  if (!s_info.mounted || !validRelSdPath(relPath) || !dst || dstLen == 0) return 0;
  File f = SD_MMC.open(relPath, "r");
  if (!f) return 0;
  size_t sz = f.size();
  if (sz == 0 || sz > dstLen) { f.close(); return 0; }
  size_t got = f.read(dst, sz);
  f.close();
  return got;
}

#else  // !BOARD_HAS_SDMMC

bool hwSdInit() { return false; }
bool hwSdMounted() { return false; }
HwSdInfo hwSdInfo() { return HwSdInfo{}; }
void hwSdPrintStatus(bool) { Serial.println("{\"sd\":{\"mounted\":false,\"err\":\"unsupported\"}}"); }
void hwSdRunSelfTest() { hwSdPrintStatus(false); }
bool hwSdFormat() { Serial.println("{\"sdformat\":{\"ok\":false,\"err\":\"unsupported\"}}"); return false; }
bool hwSdMkdir(const char*) { return false; }
bool hwSdWriteFileChunk(const char*, bool, const uint8_t*, size_t) { return false; }
bool hwSdExists(const char*) { return false; }
size_t hwSdFileSize(const char*) { return 0; }
size_t hwSdReadFile(const char*, uint8_t*, size_t) { return 0; }

#endif
