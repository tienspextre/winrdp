#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FREERDP_TAG="3.33.1"
SRC_DIR="$ROOT/third_party/freerdp3-src"
BUILD_DIR="$ROOT/third_party/freerdp3-build"
INSTALL_DIR="$ROOT/third_party/freerdp3-install"

if [[ ! -d "$SRC_DIR/.git" ]]; then
  echo "==> Cloning FreeRDP $FREERDP_TAG..."
  rm -rf "$SRC_DIR"
  git clone --depth 1 --branch "$FREERDP_TAG" https://github.com/FreeRDP/FreeRDP.git "$SRC_DIR"
fi

echo "==> Configuring FreeRDP $FREERDP_TAG (FFmpeg + H.264/AVC444 + HWAccel/VA-API + Pulse + FUSE3)..."
cmake -S "$SRC_DIR" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
  -DCMAKE_INSTALL_LIBDIR=lib \
  -DCMAKE_INSTALL_RPATH='$ORIGIN' \
  -DWITH_CCACHE=ON \
  -DWITH_CLIENT=ON \
  -DWITH_CLIENT_COMMON=ON \
  -DWITH_CLIENT_CHANNELS=ON \
  -DWITH_CLIENT_SDL=OFF \
  -DWITH_SERVER=OFF \
  -DWITH_SAMPLE=OFF \
  -DWITH_MANPAGES=OFF \
  -DBUILD_TESTING=OFF \
  -DWITH_FFMPEG=ON \
  -DWITH_FFMPEG_HWACCEL=ON \
  -DWITH_VIDEO_FFMPEG=ON \
  -DWITH_DSP_FFMPEG=ON \
  -DWITH_SWSCALE=ON \
  -DWITH_CAIRO=ON \
  -DWITH_PULSE=ON \
  -DWITH_ALSA=ON \
  -DWITH_CUPS=ON \
  -DWITH_FUSE=ON \
  -DWITH_JPEG=ON \
  -DWITH_X11=ON \
  -DWITH_WAYLAND=OFF

echo "==> Building FreeRDP $FREERDP_TAG with $(nproc) jobs..."
cmake --build "$BUILD_DIR" -j"$(nproc)"

echo "==> Installing FreeRDP $FREERDP_TAG into $INSTALL_DIR..."
cmake --install "$BUILD_DIR"

echo "==> Verifying FreeRDP build flags..."
strings "$INSTALL_DIR/lib/libfreerdp3.so" | grep -oE 'WITH_(FFMPEG|FFMPEG_HWACCEL|VIDEO_FFMPEG|GFX_H264|SWSCALE|PULSE|FUSE)=[A-Z]+' | sort -u
ls -la "$INSTALL_DIR/lib/pkgconfig/freerdp3.pc" "$INSTALL_DIR/lib/pkgconfig/freerdp-client3.pc"
