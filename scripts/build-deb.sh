#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION=""
DISTRO_TAG=""
DISTRO_TAG_SET=0
REBUILD=0
FREERDP_LIB_DIR="$ROOT/third_party/freerdp3-install/lib"
BUILD_DIR="$ROOT/build"
DIST_DIR="$ROOT/dist"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --version)
      [[ $# -ge 2 ]] || { echo "ERROR: --version requires an argument" >&2; exit 1; }
      VERSION="$2"
      shift 2
      ;;
    --distro-tag)
      [[ $# -ge 2 ]] || { echo "ERROR: --distro-tag requires an argument" >&2; exit 1; }
      DISTRO_TAG="$2"
      DISTRO_TAG_SET=1
      shift 2
      ;;
    --no-distro-tag)
      DISTRO_TAG=""
      DISTRO_TAG_SET=1
      shift
      ;;
    --freerdp-lib-dir)
      [[ $# -ge 2 ]] || { echo "ERROR: --freerdp-lib-dir requires an argument" >&2; exit 1; }
      FREERDP_LIB_DIR="$2"
      shift 2
      ;;
    --build-dir)
      [[ $# -ge 2 ]] || { echo "ERROR: --build-dir requires an argument" >&2; exit 1; }
      BUILD_DIR="$2"
      shift 2
      ;;
    --dist-dir)
      [[ $# -ge 2 ]] || { echo "ERROR: --dist-dir requires an argument" >&2; exit 1; }
      DIST_DIR="$2"
      shift 2
      ;;
    --rebuild)
      REBUILD=1
      shift
      ;;
    -h|--help)
      echo "Usage: $0 [--version <VER>] [--distro-tag <tag> | --no-distro-tag] [--freerdp-lib-dir <dir>] [--build-dir <dir>] [--dist-dir <dir>] [--rebuild]"
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

if [[ "$DISTRO_TAG_SET" -eq 0 && -r /etc/os-release ]]; then
  OS_ID="$(. /etc/os-release && printf '%s' "${ID:-}")"
  OS_VER="$(. /etc/os-release && printf '%s' "${VERSION_ID:-}")"
  if [[ "$OS_ID" == "ubuntu" && -n "$OS_VER" ]]; then
    DISTRO_TAG="ubuntu${OS_VER}"
  fi
fi

ARCH="$(dpkg --print-architecture)"

if [[ ! -e "$FREERDP_LIB_DIR/libfreerdp3.so.3" ]]; then
  echo "ERROR: Bundled FreeRDP 3.33.1 not found in $FREERDP_LIB_DIR. Run scripts/build-freerdp3.sh first." >&2
  exit 1
fi

if [[ "$REBUILD" -eq 1 || ! -x "$BUILD_DIR/winrdp" ]]; then
  "$ROOT/scripts/build-app.sh"
fi

SHLIB_WORK="$(mktemp -d)"
STAGE="$SHLIB_WORK/staging"

cleanup() {
  rm -rf "$SHLIB_WORK"
}
trap cleanup EXIT

mkdir -p "$DIST_DIR" "$STAGE"

echo "==> Installing WinRDP into staging directory..."
DESTDIR="$STAGE" cmake --install "$BUILD_DIR" --prefix /usr

BUNDLED_LIB_DIR="$STAGE/usr/lib/winrdp/lib"
mkdir -p "$BUNDLED_LIB_DIR"
cp -a \
  "$FREERDP_LIB_DIR"/libfreerdp3.so.3* \
  "$FREERDP_LIB_DIR"/libfreerdp-client3.so.3* \
  "$FREERDP_LIB_DIR"/libwinpr3.so.3* \
  "$FREERDP_LIB_DIR"/libwinpr-tools3.so.3* \
  "$BUNDLED_LIB_DIR/"

chmod 0755 "$STAGE/usr/bin/winrdp"
strip --strip-all "$STAGE/usr/bin/winrdp"

mapfile -t BUNDLED_SO_FILES < <(find "$BUNDLED_LIB_DIR" -maxdepth 1 -type f -name '*.so.*' | sort)
if [[ "${#BUNDLED_SO_FILES[@]}" -eq 0 ]]; then
  echo "ERROR: No bundled FreeRDP shared libraries found in $BUNDLED_LIB_DIR" >&2
  exit 1
fi
for so_file in "${BUNDLED_SO_FILES[@]}"; do
  chmod 0644 "$so_file"
  strip --strip-unneeded "$so_file"
done

mkdir -p "$STAGE/usr/share/applications"
sed "s|@WINRDP_BIN@|/usr/bin/winrdp|g" "$ROOT/data/io.winrdp.Client.desktop.in" \
  > "$STAGE/usr/share/applications/io.winrdp.Client.desktop"
chmod 0644 "$STAGE/usr/share/applications/io.winrdp.Client.desktop"

if command -v desktop-file-validate >/dev/null 2>&1; then
  desktop-file-validate "$STAGE/usr/share/applications/io.winrdp.Client.desktop"
fi

mkdir -p "$STAGE/usr/share/doc/winrdp"
{
  echo "Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/"
  echo "Upstream-Name: WinRDP"
  echo "Source: https://github.com/tienspextre/winrdp"
  echo ""
  echo "Files: *"
  echo "Copyright: 2026 Nguyen Dang Tien"
  echo "License: Apache-2.0"
  echo " Notice: This binary package also bundles unmodified FreeRDP 3.33.1"
  echo " shared libraries (libfreerdp3, libfreerdp-client3, libwinpr3, libwinpr-tools3)"
  echo " built with FFmpeg H.264/AVC444 and VA-API hardware decode support under /usr/lib/winrdp/lib/."
  echo ""
  cat "$ROOT/LICENSE"
} > "$STAGE/usr/share/doc/winrdp/copyright"
chmod 0644 "$STAGE/usr/share/doc/winrdp/copyright"

echo "==> Verifying ELF RPATH/RUNPATH..."
BIN_RPATH_OUT="$(readelf -d "$STAGE/usr/bin/winrdp" | grep -E 'RPATH|RUNPATH' || true)"
if [[ "$BIN_RPATH_OUT" != *'[$ORIGIN/../lib/winrdp/lib]'* ]]; then
  echo "ERROR: Unexpected RPATH/RUNPATH in staged binary:" >&2
  echo "$BIN_RPATH_OUT" >&2
  exit 1
fi
if echo "$BIN_RPATH_OUT" | grep -qE '/home/|third_party'; then
  echo "ERROR: Staged binary RPATH contains build host paths:" >&2
  echo "$BIN_RPATH_OUT" >&2
  exit 1
fi

for so_file in "${BUNDLED_SO_FILES[@]}"; do
  SO_RPATH_OUT="$(readelf -d "$so_file" | grep -E 'RPATH|RUNPATH' || true)"
  if [[ "$SO_RPATH_OUT" != *'[$ORIGIN'* ]] || echo "$SO_RPATH_OUT" | grep -qE '/home/|third_party|/opt/'; then
    echo "ERROR: Unexpected RPATH/RUNPATH in bundled library $so_file:" >&2
    echo "$SO_RPATH_OUT" >&2
    exit 1
  fi
done

echo "==> Resolving runtime dependencies with dpkg-shlibdeps..."
mkdir -p "$SHLIB_WORK/debian"
cat > "$SHLIB_WORK/debian/control" <<'EOF'
Source: winrdp
Maintainer: Nguyen Dang Tien <98021264+tienspextre@users.noreply.github.com>

Package: winrdp
Architecture: any
EOF

cat > "$SHLIB_WORK/debian/shlibs.local" <<'EOF'
libfreerdp3 3 winrdp
libfreerdp-client3 3 winrdp
libwinpr3 3 winrdp
libwinpr-tools3 3 winrdp
EOF

SHLIBS_OUT="$(cd "$SHLIB_WORK" && dpkg-shlibdeps -O -xwinrdp -l"$BUNDLED_LIB_DIR" "$STAGE/usr/bin/winrdp" "${BUNDLED_SO_FILES[@]}")"
SHLIBS_DEPENDS="$(printf '%s\n' "$SHLIBS_OUT" | sed -n 's/^shlibs:Depends=//p' | head -n 1)"

if [[ -z "$SHLIBS_DEPENDS" ]]; then
  echo "ERROR: dpkg-shlibdeps failed to compute shlibs:Depends" >&2
  echo "$SHLIBS_OUT" >&2
  exit 1
fi
if echo "$SHLIBS_DEPENDS" | grep -qE '\b(libfreerdp3|libfreerdp-client3|libwinpr3|winrdp)\b'; then
  echo "ERROR: Bundled FreeRDP/WinRDP packages leaked into Depends: $SHLIBS_DEPENDS" >&2
  exit 1
fi

INSTALLED_SIZE="$(du -sk "$STAGE" | awk '{print $1}')"

mkdir -p "$STAGE/DEBIAN"
cat > "$STAGE/DEBIAN/control" <<EOF
Package: winrdp
Version: ${VERSION}
Section: net
Priority: optional
Architecture: ${ARCH}
Installed-Size: ${INSTALLED_SIZE}
Maintainer: Nguyen Dang Tien <98021264+tienspextre@users.noreply.github.com>
Homepage: https://github.com/tienspextre/winrdp
Depends: ${SHLIBS_DEPENDS}
Recommends: intel-media-va-driver-non-free | intel-media-va-driver, mesa-va-drivers, gnome-keyring
Description: Windows-native Remote Desktop client for Ubuntu (FreeRDP 3 + OpenGL)
 WinRDP is a native GTK4/Libadwaita and OpenGL 3.3 Remote Desktop (RDP) client
 built on FreeRDP 3.33.1 with H.264/AVC444 and VA-API hardware decoding support
 for Ubuntu 24.04 LTS, 26.04 LTS, and 26.10 (X11 & Wayland).
 .
 Features a single-window zero-flicker compositor surface, Windows mstsc.exe
 style auto-hide/pinned/hidden connection bar, raw scancode keyboard grab,
 dynamic resolution, bidirectional file/image clipboard, audio/mic redirection,
 drive sharing, multi-monitor fullscreen, and GNOME Keyring integration.
EOF

cat > "$STAGE/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = "configure" ]; then
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database -q /usr/share/applications || true
    fi
    if command -v update-mime-database >/dev/null 2>&1; then
        update-mime-database /usr/share/mime || true
    fi
    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -f -t -q /usr/share/icons/hicolor || true
    fi
fi
exit 0
EOF

cat > "$STAGE/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = "remove" ] || [ "$1" = "purge" ]; then
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database -q /usr/share/applications || true
    fi
    if command -v update-mime-database >/dev/null 2>&1; then
        update-mime-database /usr/share/mime || true
    fi
    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -f -t -q /usr/share/icons/hicolor || true
    fi
fi
exit 0
EOF

chmod 0755 "$STAGE/DEBIAN/postinst" "$STAGE/DEBIAN/postrm"

if [[ -n "$DISTRO_TAG" ]]; then
  DEB_NAME="winrdp_${VERSION}_${DISTRO_TAG}_${ARCH}.deb"
else
  DEB_NAME="winrdp_${VERSION}_${ARCH}.deb"
fi
DEB_PATH="$DIST_DIR/$DEB_NAME"
rm -f "$DEB_PATH"

echo "==> Building package $DEB_PATH..."
dpkg-deb --root-owner-group --build "$STAGE" "$DEB_PATH"

(
  cd "$DIST_DIR"
  mapfile -t VER_DEBS < <(compgen -G "winrdp_${VERSION}_*.deb" | LC_ALL=C sort)
  sha256sum "${VER_DEBS[@]}" > SHA256SUMS.txt
)

echo "==> Package built successfully:"
echo "    DEB:    $DEB_PATH"
echo "    SHA256: $DIST_DIR/SHA256SUMS.txt"
cat "$DIST_DIR/SHA256SUMS.txt"
