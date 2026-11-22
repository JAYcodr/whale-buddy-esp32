#!/bin/bash
# Host unit test for the pure AOD policy (no board needed).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/tools/sim/build"
mkdir -p "$BUILD_DIR"

clang++ -std=c++17 -O2 -Wall -Wextra -Werror \
  -I"$ROOT/src" \
  "$ROOT/tools/test_aod_model.cpp" \
  -o "$BUILD_DIR/test_aod_model"

"$BUILD_DIR/test_aod_model"
