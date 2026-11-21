#!/bin/bash
# PlatformIO 包装脚本：自动定位本机 .platformio 核心目录（绕开坏掉的 ~/.platformio 符号链接）。
#
# 用法：
#   tools/pio.sh run -e waveshare-esp32s3-touch-amoled-1-8
#   tools/pio.sh run -t upload
#   tools/pio.sh run -t uploadfs
#   tools/pio.sh device list

set -euo pipefail

CORE="${PIO_CORE_DIR:-}"
if [ -z "$CORE" ]; then
  for cand in \
    "/Volumes/hamelin-ssd/devtools/.platformio" \
    "/Volumes/hamelin-ssd/01-项目工程/devtools/.platformio"
  do
    if [ -d "$cand" ]; then
      CORE="$cand"
      break
    fi
  done
fi

if [ -z "$CORE" ] || [ ! -d "$CORE" ]; then
  echo "错误：未找到 PlatformIO 核心目录（已检查 devtools/.platformio 与 01-项目工程/devtools/.platformio）" >&2
  echo "如已移到别处，用 PIO_CORE_DIR=/新路径 覆盖。" >&2
  exit 1
fi

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_DIR"

export PLATFORMIO_CORE_DIR="$CORE"
export PYTHONPATH="$CORE/penv/lib/python3.11/site-packages${PYTHONPATH:+:$PYTHONPATH}"

exec "$CORE/python3/bin/python3" -m platformio "$@"
