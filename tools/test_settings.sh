#!/bin/bash
# 卡 3 等价性测试：旧 settingsLoad() vs 表驱动新实现（10,000 组随机 NVS 输入）。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/tools/sim/build"
STUBS="$ROOT/tools/test_stubs"
mkdir -p "$BUILD_DIR"

# settings.cpp 不定义 NOVA_HOST_SIM：要编进真实 Preferences 路径下的
# settingsLoad()/settingsSave()。Preferences 与 Arduino 由 test_stubs 提供。
clang++ -std=c++17 -O2 -Wall -Wextra -Werror \
  -I"$ROOT/src" -I"$STUBS" \
  -c "$ROOT/src/model/settings.cpp" -o "$BUILD_DIR/test_settings_settings.o"

clang++ -std=c++17 -O2 -Wall -Wextra -Werror \
  -I"$ROOT/src" -I"$STUBS" \
  "$ROOT/tools/test_settings_model.cpp" \
  "$BUILD_DIR/test_settings_settings.o" \
  -o "$BUILD_DIR/test_settings"

"$BUILD_DIR/test_settings"
