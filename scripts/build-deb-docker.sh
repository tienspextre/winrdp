#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UBUNTU_VER="26.04"
VERSION=""
VERIFY_ON="ubuntu:26.04,ubuntu:26.10"
NO_CACHE=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --ubuntu)
      [[ $# -ge 2 ]] || { echo "ERROR: --ubuntu requires an argument" >&2; exit 1; }
      UBUNTU_VER="$2"
      shift 2
      ;;
    --version)
      [[ $# -ge 2 ]] || { echo "ERROR: --version requires an argument" >&2; exit 1; }
      VERSION="$2"
      shift 2
      ;;
    --verify-on)
      [[ $# -ge 2 ]] || { echo "ERROR: --verify-on requires an argument" >&2; exit 1; }
      VERIFY_ON="$2"
      shift 2
      ;;
    --no-cache)
      NO_CACHE=1
      shift
      ;;
    -h|--help)
      echo "Usage: $0 [--ubuntu 26.04|24.04|26.10] [--version <VER>] [--verify-on <img1,img2>] [--no-cache]"
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

if ! command -v docker >/dev/null 2>&1; then
  echo "ERROR: docker is required to build .deb packages for other Ubuntu releases." >&2
  exit 1
fi

ARCH="$(dpkg --print-architecture)"
DISTRO_TAG="ubuntu${UBUNTU_VER}"
DEB_NAME="winrdp_${VERSION}_${DISTRO_TAG}_${ARCH}.deb"
BUILDER_IMG="winrdp-builder:${DISTRO_TAG}"

CTX_DIR="$(mktemp -d)"
cleanup() {
  rm -rf "$CTX_DIR"
}
trap cleanup EXIT

cp "$ROOT/scripts/build-freerdp3.sh" "$CTX_DIR/build-freerdp3.sh"
chmod +x "$CTX_DIR/build-freerdp3.sh"

if [[ -f "$ROOT/third_party/freerdp3-src/CMakeLists.txt" && -d "$ROOT/third_party/freerdp3-src/.git" ]]; then
  echo "==> Exporting local FreeRDP 3.33.1 source archive for Docker builder..."
  git -C "$ROOT/third_party/freerdp3-src" archive --format=tar --prefix=freerdp3-src/ HEAD \
    | gzip > "$CTX_DIR/freerdp3-src.tar.gz"
else
  mkdir -p "$CTX_DIR/empty-src"
  tar -czf "$CTX_DIR/freerdp3-src.tar.gz" -C "$CTX_DIR/empty-src" .
fi

cat > "$CTX_DIR/Dockerfile" <<EOF
FROM ubuntu:${UBUNTU_VER}
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates build-essential cmake ninja-build pkg-config git ccache \
    libgtk-4-dev libadwaita-1-dev libsecret-1-dev libepoxy-dev libgl-dev \
    libssl-dev libx11-dev libxext-dev libxinerama-dev libxcursor-dev \
    libxfixes-dev libxi-dev libxrandr-dev libxkbfile-dev libxdamage-dev \
    libxv-dev libxrender-dev libwayland-dev libxkbcommon-dev \
    libasound2-dev libpulse-dev libcups2-dev libfuse3-dev libusb-1.0-0-dev \
    libkrb5-dev libicu-dev libcjson-dev libjson-c-dev libjpeg-turbo8-dev \
    libcairo2-dev libopus-dev liburiparser-dev libpkcs11-helper1-dev \
    libpcsclite-dev libavcodec-dev libavformat-dev libavutil-dev \
    libswscale-dev libswresample-dev libva-dev libdrm-dev \
    desktop-file-utils dpkg-dev binutils file \
    && rm -rf /var/lib/apt/lists/*
COPY build-freerdp3.sh /opt/build-freerdp3.sh
COPY freerdp3-src.tar.gz /tmp/freerdp3-src.tar.gz
RUN mkdir -p /opt/src-extract && tar -xzf /tmp/freerdp3-src.tar.gz -C /opt/src-extract && \
    rm -f /tmp/freerdp3-src.tar.gz && \
    if [ -f /opt/src-extract/freerdp3-src/CMakeLists.txt ]; then \
      mv /opt/src-extract/freerdp3-src /opt/freerdp3-src; \
    else \
      rm -rf /opt/freerdp3-src; \
    fi && \
    rm -rf /opt/src-extract && \
    FREERDP_SRC_DIR=/opt/freerdp3-src \
    FREERDP_BUILD_DIR=/tmp/freerdp3-build \
    FREERDP_INSTALL_DIR=/opt/freerdp3-install \
    /opt/build-freerdp3.sh && \
    rm -rf /opt/freerdp3-src /tmp/freerdp3-build
EOF

NEED_BUILD=0
if [[ "$NO_CACHE" -eq 1 ]] || ! docker image inspect "$BUILDER_IMG" >/dev/null 2>&1; then
  NEED_BUILD=1
fi

if [[ "$NEED_BUILD" -eq 1 ]]; then
  echo "==> Building cached Docker builder image $BUILDER_IMG (includes FreeRDP 3.33.1)..."
  BUILD_ARGS=(-t "$BUILDER_IMG")
  if [[ "$NO_CACHE" -eq 1 ]]; then
    BUILD_ARGS+=(--no-cache)
  fi
  docker build "${BUILD_ARGS[@]}" "$CTX_DIR"
else
  echo "==> Reusing cached Docker builder image $BUILDER_IMG (use --no-cache to rebuild)."
fi

mkdir -p "$ROOT/dist"
HOST_UID="$(id -u)"
HOST_GID="$(id -g)"

echo "==> Compiling, testing, and packaging WinRDP $VERSION for Ubuntu $UBUNTU_VER..."
docker run --rm \
  -e VERSION="$VERSION" \
  -e DISTRO_TAG="$DISTRO_TAG" \
  -e DEB_NAME="$DEB_NAME" \
  -e HOST_UID="$HOST_UID" \
  -e HOST_GID="$HOST_GID" \
  -v "$ROOT:/src-ro:ro" \
  -v "$ROOT/dist:/dist" \
  "$BUILDER_IMG" \
  bash -c '
    set -euo pipefail
    mkdir -p /work/repo
    cp -a /src-ro/CMakeLists.txt /src-ro/LICENSE /src-ro/README.md \
      /src-ro/src /src-ro/data /src-ro/tests /src-ro/scripts /work/repo/
    cmake -S /work/repo -B /work/repo/build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DFREERDP3_INSTALL_DIR=/opt/freerdp3-install
    cmake --build /work/repo/build -j"$(nproc)"
    ctest --test-dir /work/repo/build --output-on-failure
    /work/repo/scripts/build-deb.sh \
      --version "$VERSION" \
      --distro-tag "$DISTRO_TAG" \
      --freerdp-lib-dir /opt/freerdp3-install/lib \
      --build-dir /work/repo/build \
      --dist-dir /dist
    chown "${HOST_UID}:${HOST_GID}" "/dist/$DEB_NAME" /dist/SHA256SUMS.txt
  '

if [[ -n "$VERIFY_ON" ]]; then
  IFS=',' read -r -a VERIFY_IMAGES <<< "$VERIFY_ON"
  for img in "${VERIFY_IMAGES[@]}"; do
    img="${img// /}"
    [[ -n "$img" ]] || continue
    echo "==> Verifying clean .deb installation in container: $img"
    docker run --rm \
      -e DEBIAN_FRONTEND=noninteractive \
      -e VERSION="$VERSION" \
      -e DEB_NAME="$DEB_NAME" \
      -v "$ROOT/dist:/dist:ro" \
      "$img" \
      bash -c '
        set -euo pipefail
        apt-get update -qq
        apt-get install -y -qq --no-install-recommends "/dist/$DEB_NAME" desktop-file-utils >/dev/null
        echo "--- ldd check on /usr/bin/winrdp ---"
        if ldd /usr/bin/winrdp | grep -q "not found"; then
          ldd /usr/bin/winrdp
          exit 1
        fi
        for libname in libfreerdp3.so.3 libfreerdp-client3.so.3 libwinpr3.so.3; do
          resolved="$(ldd /usr/bin/winrdp | awk -v l="$libname" '\''$1 == l {print $3}'\'')"
          resolved_real="$(realpath -m "$resolved")"
          if [[ "$resolved_real" != "/usr/lib/winrdp/lib/"* ]]; then
            echo "ERROR: $libname did not resolve to bundled dir: $resolved ($resolved_real)" >&2
            exit 1
          fi
          echo "  $libname -> $resolved_real"
        done
        desktop-file-validate /usr/share/applications/io.winrdp.Client.desktop
        VER_OUT="$(winrdp --version)"
        echo "winrdp --version -> $VER_OUT"
        [[ "$VER_OUT" == *"$VERSION"* ]] || { echo "Version mismatch: $VER_OUT"; exit 1; }
        [[ "$VER_OUT" == *"H264/HWAccel=ON"* ]] || { echo "Missing H264/HWAccel=ON: $VER_OUT"; exit 1; }
      '
  done
fi

echo "==> Docker build & verification complete: $ROOT/dist/$DEB_NAME"
