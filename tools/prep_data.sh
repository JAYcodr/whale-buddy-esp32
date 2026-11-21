#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
rm -rf "$ROOT/data/pets/nova"
mkdir -p "$ROOT/data/pets/nova"
cp "$ROOT/assets/pets/nova/"*.nva "$ROOT/data/pets/nova/"
ls -lh "$ROOT/data/pets/nova"
