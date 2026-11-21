#!/bin/bash
# PlatformIO 包装脚本：绕开 ~/.platformio 这个坏掉的符号链接。
#
# 本机情况（2026-11 核实）：
#   ~/.platformio -> /Volumes/hamelin-ssd/devtools/.platformio   ← 目标不存在
#   真实位置：/Volumes/hamelin-ssd/01-项目工程/devtools/.platformio
#   所以 pio 命令、以及 penv 里的 python 解释器都不可直接用。
#
# 用法：
#   tools/pio.sh run -e waveshare-esp32s3-touch-amoled-1-8
#   tools/pio.sh run -t upload
#   tools/pio.sh run -t uploadfs
#   tools/pio.sh run -t erase
#   tools/pio.sh device list
#   tools/pio.sh lib list

set -euo pipefail

CORE="${PIO_CORE_DIR:-/Volumes/hamelin-ssd/01-项目工程/devtools/.platformio}"

if [ ! -d "$CORE" ]; then
  echo "错误：PlatformIO 核心目录不存在：$CORE" >&2
  echo "如已移到别处，用 PIO_CORE_DIR=/新路径 覆盖。" >&2
  exit 1
fi

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_DIR"

export PLATFORMIO_CORE_DIR="$CORE"
export PYTHONPATH="$CORE/penv/lib/python3.11/site-packages${PYTHONPATH:+:$PYTHONPATH}"

exec "$CORE/python3/bin/python3" -m platformio "$@"
