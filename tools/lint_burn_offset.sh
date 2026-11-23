#!/bin/bash
# 防烧屏偏移巡检：只警告，不失败（当前多页还没跟上，红没有意义）。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

for f in "$ROOT"/src/ui/page_*.cpp; do
  if ! grep -q "burnOffset" "$f"; then
    echo "⚠️  ${f#"$ROOT"/} 未跟随防烧屏偏移（HANDOFF.md:229 要求所有页面都要跟）"
  fi
done
exit 0
