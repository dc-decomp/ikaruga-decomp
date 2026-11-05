#!/usr/bin/env bash
# Build the Linux AppImage from an existing build directory:
#
#   cmake --build build-port --target appimage
#
# The game and launcher stay in the AppImage; the user supplies a GDI or CHD. Extracted disc files
# and the CHD cache stay in the user's data directory at run time.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${1:-$ROOT/build-port}"
BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"

if [[ ! -x "$BUILD_DIR/ikaruga" || ! -x "$BUILD_DIR/ikaruga-launcher" ]]; then
    echo "make_appimage: build ikaruga and ikaruga-launcher first" >&2
    exit 1
fi

APPDIR="$BUILD_DIR/Ikaruga.AppDir"
OUTPUT="$BUILD_DIR/Ikaruga-x86_64.AppImage"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" "$APPDIR/usr/share/applications" \
         "$APPDIR/usr/share/icons/hicolor/scalable/apps" \
         "$APPDIR/usr/share/icons/hicolor/256x256/apps" "$APPDIR/usr/share/licenses/ikaruga"

install -m 755 "$BUILD_DIR/ikaruga" "$APPDIR/usr/bin/ikaruga"
install -m 755 "$BUILD_DIR/ikaruga-launcher" "$APPDIR/usr/bin/ikaruga-launcher"
install -m 644 "$ROOT/port/packaging/ikaruga.desktop" "$APPDIR/ikaruga.desktop"
install -m 644 "$ROOT/port/packaging/ikaruga.desktop" "$APPDIR/usr/share/applications/ikaruga.desktop"
install -m 644 "$ROOT/port/packaging/ikaruga.svg" "$APPDIR/ikaruga.svg"
install -m 644 "$ROOT/port/packaging/ikaruga.svg" \
    "$APPDIR/usr/share/icons/hicolor/scalable/apps/ikaruga.svg"
install -m 644 "$ROOT/LICENSE" "$APPDIR/usr/share/licenses/ikaruga/LICENSE"
if [[ -f "$ROOT/dream-recomp/LICENSE" ]]; then
    install -m 644 "$ROOT/dream-recomp/LICENSE" "$APPDIR/usr/share/licenses/ikaruga/LICENSE-dream-recomp"
fi
if [[ -f "$ROOT/port/third_party/imgui/LICENSE.txt" ]]; then
    install -m 644 "$ROOT/port/third_party/imgui/LICENSE.txt" \
        "$APPDIR/usr/share/licenses/ikaruga/LICENSE-ImGui.txt"
fi
for sdl_license in "$ROOT/.deps/sdl3/LICENSE.txt" "$ROOT/.deps/sdl3/share/licenses/SDL3/LICENSE.txt"; do
    if [[ -f "$sdl_license" ]]; then
        install -m 644 "$sdl_license" "$APPDIR/usr/share/licenses/ikaruga/LICENSE-SDL3.txt"
        break
    fi
done
if [[ -f "$ROOT/port/packaging/ikaruga.png" ]]; then
    install -m 644 "$ROOT/port/packaging/ikaruga.png" "$APPDIR/ikaruga.png"
    install -m 644 "$ROOT/port/packaging/ikaruga.png" \
        "$APPDIR/usr/share/icons/hicolor/256x256/apps/ikaruga.png"
    ln -sf ikaruga.png "$APPDIR/.DirIcon"
else
    ln -sf ikaruga.svg "$APPDIR/.DirIcon"
fi

copy_shared_library() {
    local soname="$1"
    local source
    source="$(ldd "$BUILD_DIR/ikaruga" | awk -v name="$soname" '$1 == name && $2 == "=>" {print $3; exit}')"
    if [[ -z "$source" ]]; then
        source="$(ldd "$BUILD_DIR/ikaruga-launcher" | awk -v name="$soname" '$1 == name && $2 == "=>" {print $3; exit}')"
    fi
    if [[ -z "$source" || ! -f "$source" ]]; then
        echo "make_appimage: cannot find $soname" >&2
        exit 1
    fi
    cp -L "$source" "$APPDIR/usr/lib/$soname"
}

# SDL3 is part of the port; the C++ runtime is bundled so the AppImage does not depend on the
# build machine's compiler version. The Vulkan loader and graphics driver stay on the host. The
# licenses of the bundled libraries are in usr/share/licenses/ikaruga.
copy_shared_library "libSDL3.so.0"
copy_shared_library "libstdc++.so.6"
copy_shared_library "libgcc_s.so.1"
ln -sf libSDL3.so.0 "$APPDIR/usr/lib/libSDL3.so"

if command -v patchelf >/dev/null; then
    patchelf --set-rpath '$ORIGIN/../lib' "$APPDIR/usr/bin/ikaruga"
    patchelf --set-rpath '$ORIGIN/../lib' "$APPDIR/usr/bin/ikaruga-launcher"
fi

cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
export APPDIR="$HERE"
export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$HERE/usr/bin/ikaruga-launcher" "$@"
EOF
chmod 755 "$APPDIR/AppRun"

TOOL="${APPIMAGETOOL:-$ROOT/.deps/appimagetool-x86_64.AppImage}"
if [[ ! -x "$TOOL" ]]; then
    echo "make_appimage: downloading appimagetool..." >&2
    curl -fsSL --retry 3 \
        "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage" \
        -o "$TOOL"
    chmod +x "$TOOL"
fi

rm -f "$OUTPUT"
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$TOOL" "$APPDIR" "$OUTPUT"
echo "make_appimage: wrote $OUTPUT"
