#!/bin/bash
# 模拟器画面基线：跑 sim 到临时目录，把它自己写出的 manifest.txt 与
# tools/sim/baseline.txt 比对（manifest 是 framebuffer 原始字节的 FNV-1a，
# 不是 PNG 文件哈希，跨编码器不会假报）。
#
#   tools/sim/check.sh           比对
#   tools/sim/check.sh --update  重写基线
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BASELINE="$ROOT/tools/sim/baseline.txt"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/nova-sim.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT

cd "$ROOT"
bash tools/sim/run.sh "$OUT" > /dev/null

if [ "${1:-}" = "--update" ]; then
  cp "$OUT/manifest.txt" "$BASELINE"
  echo "baseline updated: $(wc -l < "$BASELINE" | tr -d ' ') cases -> tools/sim/baseline.txt"
  exit 0
fi

if [ ! -f "$BASELINE" ]; then
  echo "缺少 tools/sim/baseline.txt；先跑 tools/sim/check.sh --update" >&2
  exit 1
fi

if ! diff -u "$BASELINE" "$OUT/manifest.txt"; then
  echo "画面基线不一致：上面的 -/+ 就是变化" >&2
  exit 1
fi
echo "sim baseline OK ($(wc -l < "$BASELINE" | tr -d ' ') cases)"
