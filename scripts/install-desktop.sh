#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_SRC="$ROOT/build/winrdp"

if [[ ! -x "$BIN_SRC" ]]; then
  echo "ERROR: $BIN_SRC not built yet. Run scripts/build-app.sh first." >&2
  exit 1
fi

BIN_DIR="$HOME/.local/bin"
DATA_DIR="${XDG_DATA_HOME:-$HOME/.local/share}"
APP_DIR="$DATA_DIR/applications"
ICON_DIR="$DATA_DIR/icons/hicolor/scalable/apps"
MIME_DIR="$DATA_DIR/mime/packages"
SHARE_DIR="$DATA_DIR/winrdp"

mkdir -p "$BIN_DIR" "$APP_DIR" "$ICON_DIR" "$MIME_DIR" "$SHARE_DIR"

ln -sfn "$BIN_SRC" "$BIN_DIR/winrdp"
cp -f "$ROOT/data/style.css" "$SHARE_DIR/style.css"
cp -f "$ROOT/data/io.winrdp.Client.svg" "$ICON_DIR/io.winrdp.Client.svg"
cp -f "$ROOT/data/winrdp-mime.xml" "$MIME_DIR/winrdp-mime.xml"

sed "s|@WINRDP_BIN@|$BIN_DIR/winrdp|g" "$ROOT/data/io.winrdp.Client.desktop.in" > "$APP_DIR/io.winrdp.Client.desktop"
chmod +x "$APP_DIR/io.winrdp.Client.desktop"

if command -v update-desktop-database >/dev/null 2>&1; then
  update-desktop-database "$APP_DIR" || true
fi
if command -v update-mime-database >/dev/null 2>&1; then
  update-mime-database "$DATA_DIR/mime" || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
  gtk-update-icon-cache -f -t "$DATA_DIR/icons/hicolor" || true
fi

if command -v desktop-file-validate >/dev/null 2>&1; then
  desktop-file-validate "$APP_DIR/io.winrdp.Client.desktop"
fi

echo "==> Installed WinRDP desktop integration:"
echo "    Binary:  $BIN_DIR/winrdp -> $BIN_SRC"
echo "    Desktop: $APP_DIR/io.winrdp.Client.desktop"
echo "    Icon:    $ICON_DIR/io.winrdp.Client.svg"
echo "    MIME:    $MIME_DIR/winrdp-mime.xml"
