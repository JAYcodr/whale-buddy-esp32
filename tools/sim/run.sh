#!/bin/bash
# 模拟器出图。必须在仓库根运行：sim 用相对路径加载 assets/pets/nova。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
OUT_DIR="$ROOT/tools/sim/out"
BUILD_DIR="$ROOT/tools/sim/build"
mkdir -p "$OUT_DIR" "$BUILD_DIR"

GFX_SRC="$ROOT/lib/GFX_Library_for_Arduino/src"
AAFONT_CPP="$ROOT/src/ui/aafont_data.cpp"
AAFONT_O="$BUILD_DIR/aafont_data.o"

if [ ! -f "$AAFONT_O" ] || [ "$AAFONT_CPP" -nt "$AAFONT_O" ]; then
  clang++ -std=c++17 -O2 -funsigned-char -DNOVA_HOST_SIM -I"$ROOT/src" -c "$AAFONT_CPP" -o "$AAFONT_O"
fi

clang++ -std=c++17 -O2 -funsigned-char -DNOVA_HOST_SIM \
  -I"$ROOT/tools/sim/compat" \
  -I"$ROOT/src" \
  -I"$GFX_SRC" \
  "$GFX_SRC/Arduino_G.cpp" \
  "$GFX_SRC/Arduino_GFX.cpp" \
  "$GFX_SRC/canvas/Arduino_Canvas.cpp" \
  "$AAFONT_O" \
  "$ROOT/src/anim/nva.cpp" \
  "$ROOT/src/model/settings.cpp" \
  "$ROOT/src/model/state.cpp" \
  "$ROOT/src/ui/theme.cpp" \
  "$ROOT/src/ui/icons_data.cpp" \
  "$ROOT/src/ui/page_home.cpp" \
  "$ROOT/src/ui/page_quota.cpp" \
  "$ROOT/src/ui/page_rank.cpp" \
  "$ROOT/src/ui/page_detail.cpp" \
  "$ROOT/src/ui/page_settings.cpp" \
  "$ROOT/src/ui/page_approval.cpp" \
  "$ROOT/src/ui/page_tokens.cpp" \
  "$ROOT/src/ui/page_chat.cpp" \
  "$ROOT/src/ui/nav.cpp" \
  "$ROOT/src/ui/transition.cpp" \
  "$ROOT/tools/sim/main.cpp" \
  -lz \
  -o "$BUILD_DIR/nova_sim"

"$BUILD_DIR/nova_sim" "${1:-$OUT_DIR}"
