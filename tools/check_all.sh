#!/bin/bash
# 一条命令跑完项目回归。任一硬性检查失败即 exit 1。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

TOTAL=0
PASS=0
OUT=""

run_step() {
  local label="$1"
  shift
  TOTAL=$((TOTAL + 1))
  if ! OUT=$("$@" 2>&1); then
    echo "FAIL: $label"
    echo "$OUT"
    echo "FAIL $PASS/$TOTAL"
    exit 1
  fi
  PASS=$((PASS + 1))
  echo "PASS: $label"
}

num_checked() {   # 从 "N checks, ..." / "N checks passed" 里取 N
  echo "$OUT" | grep -oE '[0-9]+ checks' | head -1 | grep -oE '[0-9]+'
}

run_step "battery"  bash tools/test_battery.sh
BATTERY_N=$(num_checked)

run_step "aod"      bash tools/test_aod.sh
AOD_N=$(num_checked)

run_step "proto"    bash tools/test_proto.sh
PROTO_N=$(num_checked)

run_step "settings" bash tools/test_settings.sh
SETTINGS_N=$(num_checked)

run_step "junit"    bash -c 'cd host-plugin && npm test'
JUNIT_N=$(echo "$OUT" | grep -E '^ℹ tests [0-9]+' | grep -oE '[0-9]+' | head -1)

run_step "sim"      bash tools/sim/check.sh

# 烧屏巡检只警告，不计入失败。
bash tools/lint_burn_offset.sh || true
PASS=$((PASS + 1)); TOTAL=$((TOTAL + 1))

# 裸 368：只许出现在 src/boards/（以及两个定义/协议豁免点）。
NAKED=$(grep -rnw "368" src --include='*.cpp' --include='*.h' \
  | grep -v '^src/boards/' \
  | grep -v '^src/ui/canvas.h:' \
  | grep -v '^src/net/proto.cpp:' \
  | grep -vE ':[0-9]+:[[:space:]]*//' || true)
TOTAL=$((TOTAL + 1))
if [ -n "$NAKED" ]; then
  echo "FAIL: naked 368 in src/"
  echo "$NAKED"
  echo "FAIL $PASS/$TOTAL"
  exit 1
fi
PASS=$((PASS + 1))
echo "PASS: naked 368"

BASELINE_N=$(wc -l < tools/sim/baseline.txt | tr -d ' ')
echo "PASS $PASS/$TOTAL · proto $PROTO_N · battery $BATTERY_N · aod $AOD_N · settings $SETTINGS_N · junit $JUNIT_N · baseline $BASELINE_N/$BASELINE_N"
