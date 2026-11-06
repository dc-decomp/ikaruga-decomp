#!/usr/bin/env bash
# Build the Linux AppImage inside an Ubuntu 24.04 container (glibc 2.39), so it starts on distributions
# older than the machine you build on. The AppImage bundles SDL3 and the C++ runtime but not libc, so
# the glibc it needs is the glibc it was built against. A host with a newer glibc than the Steam Deck's
# makes an AppImage the Deck cannot start.
#
#   tools/linux/build_appimage_compat.sh            # podman (or docker) is required
#
# Needs decomp/ (the generated code) like any other build. Writes build-compat/Ikaruga-x86_64.AppImage
# and leaves build-port, .deps/sdl3 and the host toolchain alone: the container builds its own SDL3
# into .deps/sdl3-compat and the port into build-compat. The first run installs packages and builds
# SDL3 and the decompiled code, which takes several minutes; the container is removed afterwards.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ENGINE="${CONTAINER_ENGINE:-$(command -v podman || command -v docker || true)}"
IMAGE="${COMPAT_IMAGE:-docker.io/library/ubuntu:24.04}"
JOBS="${JOBS:-$(nproc)}"

if [[ -z "$ENGINE" ]]; then
    echo "build_appimage_compat: install podman or docker" >&2
    exit 1
fi
if [[ ! -d "$ROOT/decomp/gen" ]]; then
    echo "build_appimage_compat: decomp/ is missing; generate it first (see README)" >&2
    exit 1
fi
SDL_SRC="$(ls -d "$ROOT"/.deps/SDL3-* 2>/dev/null | sort -V | tail -1 || true)"
if [[ -z "$SDL_SRC" ]]; then
    echo "build_appimage_compat: no SDL3 source in .deps/ (SDL3-x.y.z); unpack one there" >&2
    exit 1
fi

# The repository is mounted at its own path, so absolute paths in the build match the host's.
# Rootless podman maps the container's root to you, so the files it writes are yours.
USERNS=()
[[ "$ENGINE" == *docker* ]] && USERNS=(--user "$(id -u):$(id -g)")

"$ENGINE" run --rm "${USERNS[@]}" -v "$ROOT:$ROOT" -w "$ROOT" \
    -e "JOBS=$JOBS" -e "SDL_SRC=$SDL_SRC" -e "HOME=/tmp" \
    "$IMAGE" bash -euo pipefail -c '
if [[ "$(id -u)" == 0 ]]; then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends \
        build-essential gcc-14 g++-14 cmake ninja-build pkg-config git ca-certificates curl file \
        patchelf squashfs-tools python3 glslc libvulkan-dev zlib1g-dev \
        libasound2-dev libpulse-dev libpipewire-0.3-dev libsndio-dev libudev-dev libdbus-1-dev \
        libx11-dev libxext-dev libxcursor-dev libxi-dev libxfixes-dev libxrandr-dev libxrender-dev \
        libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev \
        libdrm-dev libgbm-dev libegl-dev libgl-dev libibus-1.0-dev >/dev/null
fi
export CC=gcc-14 CXX=g++-14
ldd --version | head -1

SDL_PREFIX="$PWD/.deps/sdl3-compat"
if [[ ! -f "$SDL_PREFIX/lib/cmake/SDL3/SDL3Config.cmake" ]]; then
    cmake -S "$SDL_SRC" -B .deps/sdl3-compat-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$SDL_PREFIX" -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_TESTS=OFF
    cmake --build .deps/sdl3-compat-build --parallel "$JOBS"
    cmake --install .deps/sdl3-compat-build
fi

cmake -S port -B build-compat -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$SDL_PREFIX" -DIKARUGA_DECOMP_DIR="$PWD/decomp"
cmake --build build-compat --parallel "$JOBS" --target ikaruga ikaruga-launcher
bash tools/linux/make_appimage.sh build-compat
objdump -T build-compat/ikaruga | grep -o "GLIBC_[0-9.]*" | sort -Vu | tail -1
'

echo "build_appimage_compat: wrote $ROOT/build-compat/Ikaruga-x86_64.AppImage"
