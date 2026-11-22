#pragma once
// 模拟器用的 esp_qrcode 桩。
//
// 真机上用的是核心自带的 espressif__qrcode（随 framework 分发、默认已链接），
// 这里不可能拉到那套东西，所以给一个够用的替身：只支持本项目的固定载荷
// "WIFI:T:nopass;S:<ssid>;;"，点阵是 segno 按 error=L 生成的 v2 / 25 模块。
//
// 目的是让 AP 页在模拟器里能出**真实可扫的图**，而不是一个黑方块 ——
// 布局、缩放、静区这些问题只有画真码才能看出来。
//
// 换 SSID 时要重跑生成（默认是 Nova-3468）。

#include <stdint.h>
#include <string.h>
#include <stdio.h>

typedef const uint8_t* esp_qrcode_handle_t;

typedef struct {
    void (*display_func)(esp_qrcode_handle_t qrcode);
    int max_qrcode_version;
    int qrcode_ecc_level;
} esp_qrcode_config_t;

enum {
    ESP_QRCODE_ECC_LOW,
    ESP_QRCODE_ECC_MED,
    ESP_QRCODE_ECC_QUART,
    ESP_QRCODE_ECC_HIGH,
};

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1

#define ESP_QRCODE_CONFIG_DEFAULT() (esp_qrcode_config_t) { \
    .display_func = nullptr, \
    .max_qrcode_version = 10, \
    .qrcode_ecc_level = ESP_QRCODE_ECC_LOW, \
}

namespace simqrcode {

constexpr int kSize = 25;

// segno v2 / 25 模块 / error=L，载荷 "WIFI:T:nopass;S:Nova-3468;;"
// 1 = 深色模块。
static const char* const kRows[kSize] = {
    "1111111001110010001111111",
    "1000001000011110001000001",
    "1011101001110010101011101",
    "1011101010111101101011101",
    "1011101010101011101011101",
    "1000001001101110001000001",
    "1111111010101010101111111",
    "0000000001001001000000000",
    "1100011101101110100011000",
    "1110010100101011110111101",
    "1001001111000001101111111",
    "1100000100111011001111011",
    "1011001101100011001101110",
    "1000000010111100100100101",
    "1000111011100011101010011",
    "1000110011011001111100110",
    "1010001001010100111110111",
    "0000000010011011100010111",
    "1111111010011010101011001",
    "1000001010101010100011010",
    "1011101000010101111110000",
    "1011101001111001011001111",
    "1011101000101011010101001",
    "1000001010110001101010001",
    "1111111010110010100100001",
};

}  // namespace simqrcode

inline int esp_qrcode_generate(esp_qrcode_config_t* cfg, const char* text) {
    if (!cfg || !cfg->display_func || !text) return ESP_FAIL;
    // 只认本项目这一个形状，避免模拟器悄悄给错图。
    if (strstr(text, "WIFI:T:nopass;S:") == nullptr) return ESP_FAIL;
    cfg->display_func(reinterpret_cast<esp_qrcode_handle_t>(simqrcode::kRows));
    return ESP_OK;
}

inline int esp_qrcode_get_size(esp_qrcode_handle_t) { return simqrcode::kSize; }

inline bool esp_qrcode_get_module(esp_qrcode_handle_t, int x, int y) {
    if (x < 0 || x >= simqrcode::kSize || y < 0 || y >= simqrcode::kSize) return false;
    return simqrcode::kRows[y][x] == '1';
}

inline void esp_qrcode_print_console(esp_qrcode_handle_t) {}
