#!/bin/bash
# Build multiaddonmanager with CMake for CS2 linuxsteamrt64.
#
# Usage:
#   cd multiaddonmanager && bash build_cmake.sh
#   MAM_NATIVE=1 bash build_cmake.sh   # local build (Ubuntu 20.04/22.04 only)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK_ROOT="$(cd "$ROOT/.." && pwd)"
DEPS="${DEPS_ROOT:-$WORK_ROOT/deps}"

if [ "${MAM_IN_DOCKER:-0}" = "1" ]; then
  BUILD="$ROOT/build"
else
  if [[ "$ROOT" == /mnt/* ]]; then
    BUILD="${MAM_BUILD_DIR:-$HOME/.cache/multiaddonmanager/build}"
  else
    BUILD="$ROOT/build"
  fi
fi

DOCKER_IMAGE="${MAM_DOCKER_IMAGE:-multiaddonmanager-cmake-builder}"

mam_host_glibc_ok() {
  if [ -f /etc/os-release ]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    case "${VERSION_ID:-}" in
      20.04|22.04) return 0 ;;
    esac
  fi
  if command -v ldd >/dev/null 2>&1; then
    local ver
    ver="$(ldd --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+' | head -1 || true)"
    case "$ver" in
      2.3[0-5]|2.2[0-9]|2.1[0-9]|2.[0-9]) return 0 ;;
    esac
  fi
  return 1
}

if [ "${MAM_NATIVE:-0}" != "1" ] && [ "${MAM_IN_DOCKER:-0}" != "1" ]; then
  if mam_host_glibc_ok; then
    echo "==> Ubuntu 20.04/22.04 (or GLIBC <= 2.35) detected — native build"
    MAM_NATIVE=1
  elif ! command -v docker >/dev/null 2>&1; then
    cat >&2 <<'EOF'
ERROR: Docker is required for a CS2-compatible build on this host.

Options:
  1) Install Docker, then re-run: bash build_cmake.sh
  2) Build inside Ubuntu 20.04/22.04 (WSL distro), then re-run
EOF
    exit 1
  fi
fi

if [ "${MAM_NATIVE:-0}" != "1" ] && [ "${MAM_IN_DOCKER:-0}" != "1" ]; then
  if ! docker image inspect "$DOCKER_IMAGE" >/dev/null 2>&1; then
    echo "==> Building Docker image $DOCKER_IMAGE (one-time)"
    docker build -f "$ROOT/Dockerfile.cmake" -t "$DOCKER_IMAGE" "$ROOT"
  fi

  echo "==> Building multiaddonmanager inside Docker ($DOCKER_IMAGE)"
  docker run --rm \
    -v "$WORK_ROOT:/work" \
    -e MAM_IN_DOCKER=1 \
    -e MAM_NATIVE=1 \
    "$DOCKER_IMAGE" \
    bash -lc "sed -i 's/\r$//' /work/multiaddonmanager/build_cmake.sh && bash /work/multiaddonmanager/build_cmake.sh"
  exit $?
fi

mkdir -p "$DEPS"

if command -v apt-get >/dev/null 2>&1; then
  missing=()
  command -v cmake >/dev/null 2>&1 || missing+=(cmake)
  command -v git >/dev/null 2>&1 || missing+=(git)
  command -v g++ >/dev/null 2>&1 || missing+=(build-essential)
  if [ ${#missing[@]} -gt 0 ]; then
    cat >&2 <<EOF
Missing packages: ${missing[*]}

Install once:
  sudo apt-get update
  sudo apt-get install -y build-essential cmake git
EOF
    exit 1
  fi
fi

if [ "$BUILD" != "$ROOT/build" ]; then
  echo "==> Build dir (Linux fs): $BUILD"
fi

if [ ! -f "$DEPS/hl2sdk-cs2/public/tier0/dbg.h" ]; then
  echo "==> Cloning hl2sdk-cs2"
  rm -rf "$DEPS/hl2sdk-cs2"
  git clone --branch cs2 --depth 1 https://github.com/alliedmodders/hl2sdk.git "$DEPS/hl2sdk-cs2"
fi

if [ ! -f "$DEPS/metamod-source/core/ISmmPlugin.h" ]; then
  echo "==> Cloning metamod-source"
  git clone --recursive --depth 1 https://github.com/alliedmodders/metamod-source.git "$DEPS/metamod-source"
fi

chmod +x "$DEPS/hl2sdk-cs2/devtools/bin/linux/protoc" 2>/dev/null || true

cmake -S "$ROOT" -B "$BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DHL2SDK_PATH="$DEPS/hl2sdk-cs2" \
  -DMMS_PATH="$DEPS/metamod-source"

cmake --build "$BUILD" -j"$(nproc)"

SO="$BUILD/addons/multiaddonmanager/bin/linuxsteamrt64/multiaddonmanager.so"
if [ -f "$SO" ] && command -v objdump >/dev/null 2>&1; then
  MAX_GLIBC="$(objdump -T "$SO" 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sort -u -V | tail -1 || true)"
  if [ -n "$MAX_GLIBC" ]; then
    echo "==> Max GLIBC symbol required: $MAX_GLIBC"
  fi
fi

echo "=== BUILD OK ==="
find "$BUILD/addons" "$BUILD/cfg" -type f 2>/dev/null || find "$BUILD/addons" -type f
if [ "$BUILD" != "$ROOT/build" ] && [ -d "$BUILD/addons" ]; then
  rm -rf "$ROOT/build"
  mkdir -p "$ROOT/build"
  cp -a "$BUILD/addons" "$ROOT/build/"
  if [ -d "$BUILD/cfg" ]; then
    cp -a "$BUILD/cfg" "$ROOT/build/"
  fi
  echo "==> Copied package -> $ROOT/build"
fi
