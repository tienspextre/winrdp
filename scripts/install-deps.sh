#!/usr/bin/env bash
set -euo pipefail

# Ubuntu 24.04 LTS / 26.04 LTS / 26.10 build & runtime dependencies for WinRDP + FreeRDP 3.33.1 (H.264/AVC444/VA-API)
sudo apt update
sudo apt install -y \
  build-essential cmake ninja-build pkg-config git ccache \
  libgtk-4-dev libadwaita-1-dev libsecret-1-dev libepoxy-dev libgl-dev \
  libssl-dev libx11-dev libxext-dev libxinerama-dev libxcursor-dev \
  libxfixes-dev libxi-dev libxrandr-dev libxkbfile-dev libxdamage-dev \
  libxv-dev libxrender-dev libwayland-dev libxkbcommon-dev \
  libasound2-dev libpulse-dev libcups2-dev libfuse3-dev libusb-1.0-0-dev \
  libkrb5-dev libicu-dev libcjson-dev libjson-c-dev libjpeg-turbo8-dev \
  libcairo2-dev libopus-dev liburiparser-dev libpkcs11-helper1-dev \
  libpcsclite-dev libavcodec-dev libavformat-dev libavutil-dev \
  libswscale-dev libswresample-dev libva-dev \
  vainfo libdrm-dev desktop-file-utils dpkg-dev

if apt-cache show intel-media-va-driver-non-free >/dev/null 2>&1; then
  sudo apt install -y intel-media-va-driver-non-free
else
  sudo apt install -y intel-media-va-driver
fi
