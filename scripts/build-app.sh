#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ ! -f "$ROOT/third_party/freerdp3-install/lib/pkgconfig/freerdp3.pc" ]]; then
  echo "ERROR: Bundled FreeRDP 3.33.1 not found. Run scripts/build-freerdp3.sh first." >&2
  exit 1
fi

echo "==> Configuring WinRDP..."
cmake -S "$ROOT" -B "$ROOT/build" -G Ninja -DCMAKE_BUILD_TYPE=Release

echo "==> Building WinRDP..."
cmake --build "$ROOT/build" -j"$(nproc)"

echo "==> Running unit tests..."
ctest --test-dir "$ROOT/build" --output-on-failure

echo "==> Build & tests complete: $ROOT/build/winrdp"
