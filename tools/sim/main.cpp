#include "anim/nva.h"
#include "model/settings.h"
#include "model/state.h"
#include "ui/canvas.h"
#include "ui/nav.h"
#include <canvas/Arduino_Canvas.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <zlib.h>

static void writeBe32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back((uint8_t)(v >> 24));
  out.push_back((uint8_t)(v >> 16));
  out.push_back((uint8_t)(v >> 8));
  out.push_back((uint8_t)v);
}

static void writeChunk(std::vector<uint8_t>& out, const char tag[4], const uint8_t* data, size_t len) {
  writeBe32(out, (uint32_t)len);
  size_t tagPos = out.size();
  out.insert(out.end(), tag, tag + 4);
  if (len && data) out.insert(out.end(), data, data + len);
  uLong crc = crc32(0L, Z_NULL, 0);
  crc = crc32(crc, out.data() + tagPos, (uInt)(4 + len));
  writeBe32(out, (uint32_t)crc);
}

static bool saveRgb565Png(const char* path, const uint16_t* fb, int w, int h) {
  std::vector<uint8_t> raw((size_t)h * (1 + w * 3));
  for (int y = 0; y < h; y++) {
    uint8_t* row = raw.data() + (size_t)y * (1 + w * 3);
    row[0] = 0;
    for (int x = 0; x < w; x++) {
      uint16_t c = fb[y * w + x];
      row[1 + x * 3 + 0] = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
      row[1 + x * 3 + 1] = (uint8_t)(((c >> 5) & 0x3F) * 255 / 63);
      row[1 + x * 3 + 2] = (uint8_t)((c & 0x1F) * 255 / 31);
    }
  }
  uLongf compLen = compressBound((uLong)raw.size());
  std::vector<uint8_t> comp(compLen);
  if (compress2(comp.data(), &compLen, raw.data(), (uLong)raw.size(), 6) != Z_OK) return false;

  std::vector<uint8_t> png = { 137, 80, 78, 71, 13, 10, 26, 10 };
  uint8_t ihdr[13] = {
    (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
    (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h,
    8, 2, 0, 0, 0
  };
  writeChunk(png, "IHDR", ihdr, sizeof(ihdr));
  writeChunk(png, "IDAT", comp.data(), (size_t)compLen);
  writeChunk(png, "IEND", nullptr, 0);

  FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  std::fwrite(png.data(), 1, png.size(), f);
  std::fclose(f);
  return true;
}

static void populateSample(NovaState& st) {
  std::memset(&st, 0, sizeof(st));
  std::strcpy(st.deviceId, "D93468");
  std::strcpy(st.fwVer, "0.3.0");
  std::strcpy(st.ip, "192.168.31.118");
  st.rssi = -52;
  st.wifiConnected = true;
  st.mqttConnected = true;
  st.run = 1;
  st.wait = 0;
  st.sess = 3;
  std::strcpy(st.msg, "bash: pio run -e waveshare");
  st.ctxPct = 42;
  std::strcpy(st.activeModel, "GPT-6.2 Astra");

  st.nCards = 3;
  std::strcpy(st.cards[0].id, "ds");
  std::strcpy(st.cards[0].name, "DeepSeek");
  std::strcpy(st.cards[0].kind, "bal");
  st.cards[0].val = 77.10;
  std::strcpy(st.cards[0].cur, "CNY");
  st.cards[0].ageSec = 12;

  std::strcpy(st.cards[1].id, "oc");
  std::strcpy(st.cards[1].name, "OpenCode Go");
  std::strcpy(st.cards[1].kind, "win");
  st.cards[1].ageSec = 45;
  st.cards[1].nBars = 2;
  st.cards[1].bars[0] = { 73, "w", 7200 };
  st.cards[1].bars[1] = { 66, "m", 864000 };

  std::strcpy(st.cards[2].id, "od");
  std::strcpy(st.cards[2].name, "OpenDesign");
  std::strcpy(st.cards[2].kind, "win");
  st.cards[2].ageSec = 18;
  st.cards[2].nBars = 2;
  st.cards[2].bars[0] = { 0, "5h", 18000 };
  st.cards[2].bars[1] = { 39, "w", 302400 };

  std::strcpy(st.rankSrc, "ModelDial");
  std::strcpy(st.rankLic, "CC BY 4.0");
  st.nRows = 24;
  const char* models[8] = {
    "Claude Fable 5.2", "GPT-6.2 Astra", "Gemini 4 Pro", "DeepSeek V4",
    "Qwen 3.5 Max", "o4-mini High", "Sonnet 4.6", "GLM-5 Pro"
  };
  for (int i = 0; i < 24; i++) {
    st.rows[i].r = (uint8_t)(i + 1);
    if (i < 8) std::strcpy(st.rows[i].m, models[i]);
    else std::snprintf(st.rows[i].m, sizeof(st.rows[i].m), "Model-%02d", i + 1);
    std::strcpy(st.rows[i].e, (i == 0) ? "max" : "xhigh");
    st.rows[i].isNew = (i < 2);
    st.rows[i].s = 72.9f - i * 1.1f;
    st.rows[i].b = (uint8_t)(68 - i);
    st.rows[i].f = (uint8_t)(85 - i * 2);
    st.rows[i].k = (uint8_t)(100 - i * 2);
    st.rows[i].a = (uint8_t)(58 - i);
    st.rows[i].q = (uint8_t)(75 - i);
    std::snprintf(st.rows[i].dur, sizeof(st.rows[i].dur), "%dm", 120 + i * 15);
    std::snprintf(st.rows[i].cost, sizeof(st.rows[i].cost), "$%.1f", 45.0 - i * 1.2);
  }

  st.fps = 60.0f;
  st.cpuMhz = 240;
  st.nowMs = 10000;
  st.lastMqttRxMs = 8000;
  st.burnInStep = 0;
  st.pushUs = 8600;
  st.freeHeap = 182 * 1024;
  st.freePsram = 7400 * 1024;
  st.battMv = 4120;
  st.battPct = 92;
  st.usbPresent = true;
  st.hour = 16;
  st.minute = 55;
  st.second = 10;
}

int main(int argc, char** argv) {
  std::string outDir = (argc > 1) ? argv[1] : "tools/sim/out";
  Arduino_Canvas canvas(UI_W, UI_H, nullptr);
  if (!canvas.begin(GFX_SKIP_OUTPUT_BEGIN)) return 1;
  canvas.setUTF8Print(true);
  nvaInit("assets/pets/nova");

  NovaState st;
  populateSample(st);
  NovaSettings cfg;
  UiNav nav;

  struct Case { const char* name; UiPage page; bool approval; };
  Case cases[] = {
    { "home", UiPage::HOME, false },
    { "quota", UiPage::QUOTA, false },
    { "rank", UiPage::RANK, false },
    { "detail", UiPage::DETAIL, false },
    { "settings", UiPage::SETTINGS, false },
    { "approval", UiPage::HOME, true },
  };

  for (const auto& cs : cases) {
    st.approvalActive = cs.approval;
    std::strcpy(st.apId, "ap_7f3");
    std::strcpy(st.apTool, "Bash");
    std::strcpy(st.apReason, "rm -rf /tmp/nova-cache && ./tools/pio.sh run -t uploadfs");
    st.apTtlMs = 8000;
    st.apArrivedMs = 1000;
    nav.setPage(cs.page);
    nav.draw(canvas, st, cfg, 24, 6500);
    std::string path = outDir + "/page_" + cs.name + ".png";
    if (!saveRgb565Png(path.c_str(), canvas.getFramebuffer(), UI_W, UI_H)) return 1;
    std::printf("wrote %s\n", path.c_str());
  }
  return 0;
}
