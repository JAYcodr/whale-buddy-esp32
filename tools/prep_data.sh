#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
rm -rf "$ROOT/data/pets/nova" "$ROOT/data/audio"
mkdir -p "$ROOT/data/pets/nova" "$ROOT/data/audio"
cp "$ROOT/assets/pets/nova/"*.nva "$ROOT/data/pets/nova/"
if ls "$ROOT/assets/audio/"*.adp >/dev/null 2>&1; then
  cp "$ROOT/assets/audio/"*.adp "$ROOT/data/audio/"
fi
ls -lh "$ROOT/data/pets/nova" "$ROOT/data/audio"
