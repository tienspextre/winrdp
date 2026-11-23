#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION=""
DIST_DIR="$ROOT/dist"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --version)
      [[ $# -ge 2 ]] || { echo "ERROR: --version requires an argument" >&2; exit 1; }
      VERSION="$2"
      shift 2
      ;;
    --dist-dir)
      [[ $# -ge 2 ]] || { echo "ERROR: --dist-dir requires an argument" >&2; exit 1; }
      DIST_DIR="$2"
      shift 2
      ;;
    -h|--help)
      echo "Usage: $0 [--version <VER>] [--dist-dir <dir>]"
      exit 0
      ;;
    *)
      echo "ERROR: Unknown argument: $1" >&2
      exit 1
      ;;
  esac
done

if [[ -z "$VERSION" ]]; then
  VERSION="$(sed -nE 's/^[[:space:]]*project\(winrdp[[:space:]]+VERSION[[:space:]]+([^[:space:])]+).*/\1/p' "$ROOT/CMakeLists.txt" | head -n 1)"
fi
if [[ -z "$VERSION" ]]; then
  echo "ERROR: Failed to determine version from CMakeLists.txt" >&2
  exit 1
fi

if ! command -v flatpak >/dev/null 2>&1; then
  echo "ERROR: flatpak is required to build the Flatpak bundle." >&2
  exit 1
fi

ARCH="$(uname -m)"
MANIFEST="$ROOT/flatpak/io.winrdp.Client.yml"
STATE_DIR="$ROOT/third_party/flatpak-builder-state"
BUILD_DIR="$ROOT/third_party/flatpak-build"
REPO_DIR="$ROOT/third_party/flatpak-repo"

mkdir -p "$DIST_DIR" "$STATE_DIR" "$BUILD_DIR" "$REPO_DIR"

echo "==> Ensuring Flathub remote and GNOME 50 SDK/Platform runtimes (--user)..."
flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo

ensure_ref() {
  local ref="$1"
  if flatpak info "$ref" >/dev/null 2>&1; then
    echo "    Already installed: $ref"
  else
    echo "    Installing: $ref"
    flatpak install --user -y --noninteractive flathub "$ref"
  fi
}

ensure_ref "org.gnome.Platform//50"
ensure_ref "org.gnome.Sdk//50"
ensure_ref "org.freedesktop.Platform.codecs-extra//25.08-extra"
ensure_ref "org.freedesktop.Platform.VAAPI.Intel//25.08"

BUILDER_CMD=()
if command -v flatpak-builder >/dev/null 2>&1; then
  BUILDER_CMD=(flatpak-builder)
else
  ensure_ref "org.flatpak.Builder"
  BUILDER_CMD=(flatpak run --filesystem="$ROOT" --share=network org.flatpak.Builder)
fi

echo "==> Building Flatpak repo for WinRDP $VERSION ($ARCH)..."
"${BUILDER_CMD[@]}" \
  --user \
  --force-clean \
  --disable-rofiles-fuse \
  --state-dir="$STATE_DIR" \
  --repo="$REPO_DIR" \
  "$BUILD_DIR" \
  "$MANIFEST"

BUNDLE_NAME="winrdp_${VERSION}_${ARCH}.flatpak"
BUNDLE_PATH="$DIST_DIR/$BUNDLE_NAME"
rm -f "$BUNDLE_PATH"

echo "==> Exporting single-file Flatpak bundle: $BUNDLE_PATH..."
flatpak build-bundle \
  --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo \
  "$REPO_DIR" \
  "$BUNDLE_PATH" \
  io.winrdp.Client

(
  cd "$DIST_DIR"
  mapfile -t VER_FILES < <({ compgen -G "winrdp_${VERSION}_*.flatpak" || true; compgen -G "winrdp_${VERSION}_*.deb" || true; } | LC_ALL=C sort)
  if [[ "${#VER_FILES[@]}" -gt 0 ]]; then
    sha256sum "${VER_FILES[@]}" > SHA256SUMS.txt
  fi
)

echo "==> Flatpak bundle built successfully:"
echo "    BUNDLE: $BUNDLE_PATH"
echo "    SHA256: $DIST_DIR/SHA256SUMS.txt"
cat "$DIST_DIR/SHA256SUMS.txt"
