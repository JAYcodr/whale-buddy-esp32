#!/bin/bash
# Host unit tests for src/net/proto.cpp (no board needed).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/tools/sim/build"
STUBS="$ROOT/tools/test_stubs"
AJ_SRC="$ROOT/.pio/libdeps/waveshare-esp32s3-touch-amoled-1-8/ArduinoJson/src"
mkdir -p "$BUILD_DIR"

if [ ! -d "$AJ_SRC" ]; then
  AJ_SRC="$(ls -d "$ROOT"/.pio/libdeps/*/ArduinoJson/src 2>/dev/null | head -n 1)"
fi
if [ -z "$AJ_SRC" ] || [ ! -d "$AJ_SRC" ]; then
  echo "未找到 ArduinoJson；请先跑 tools/pio.sh run 生成 .pio/libdeps" >&2
  exit 1
fi

# proto.cpp 不定义 NOVA_HOST_SIM：要编进真实的 applyTtsChunk 分片状态机。
# 它依赖的 ESP 符号由 test_stubs + test_proto.cpp 的桩补齐。
clang++ -std=c++17 -O2 -Wall -Wextra -Werror -I"$ROOT/src" -I"$STUBS" -I"$AJ_SRC" \
  -c "$ROOT/src/net/proto.cpp" -o "$BUILD_DIR/test_proto_proto.o"

# state / settings 只差一个宏：用 SIM 版本避开 FreeRTOS 与 Preferences。
clang++ -std=c++17 -O2 -Wall -Wextra -Werror \
  -DNOVA_HOST_SIM -I"$ROOT/src" -I"$ROOT/tools/sim/compat" \
  -c "$ROOT/src/model/state.cpp" -o "$BUILD_DIR/test_proto_state.o"
clang++ -std=c++17 -O2 -Wall -Wextra -Werror \
  -DNOVA_HOST_SIM -I"$ROOT/src" -I"$ROOT/tools/sim/compat" \
  -c "$ROOT/src/model/settings.cpp" -o "$BUILD_DIR/test_proto_settings.o"

clang++ -std=c++17 -O2 -Wall -Wextra -Werror \
  -I"$ROOT/src" -I"$STUBS" -I"$AJ_SRC" -I"$ROOT/tools/sim/compat" \
  "$ROOT/tools/test_proto.cpp" \
  "$BUILD_DIR/test_proto_proto.o" \
  "$BUILD_DIR/test_proto_state.o" \
  "$BUILD_DIR/test_proto_settings.o" \
  -o "$BUILD_DIR/test_proto"

"$BUILD_DIR/test_proto"
