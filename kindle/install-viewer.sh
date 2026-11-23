#!/bin/bash
set -euo pipefail
unset CDPATH

project_dir=$(cd "$(dirname "$0")/.." && pwd -P)
sdk="$HOME/.local/opt/manga-kindle-sdk"
viewer="$project_dir/build/kindle-viewer/manga_kindle"
launcher="$project_dir/kindle/Manga Viewer.sh"

usage() {
    cat <<'HELP'
Usage: ./kindle/install-viewer.sh [--sdk DIR]

Connect the Kindle in USB file-transfer mode. Close other MTP transfer apps or
unmount the MTP drive before running the installer.
Use the same SDK as build-viewer.sh (default: $HOME/.local/opt/manga-kindle-sdk).
The Mac transfer helper requires the Command Line Tools and Homebrew libmtp and pkgconf.
HELP
}

fail() { echo "$*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --sdk)
            [[ -n ${2:-} && $2 != --* ]] || fail "--sdk needs a directory"
            sdk=$2
            shift 2 ;;
        *) usage >&2; fail "Unknown argument: $1" ;;
    esac
done

[[ -f "$viewer" && -r "$viewer" ]] || fail "Missing Manga Viewer. Run ./kindle/build-viewer.sh first."
description=$(file -Lb "$viewer")
[[ "$description" == *"ELF 32-bit"*"ARM"*"/lib/ld-musl-armhf.so.1"* ]] || fail "Unexpected viewer binary at $viewer: $description"
[[ -f "$launcher" && -r "$launcher" ]] || fail "Missing launcher: $launcher"
[[ -f "$sdk/kindle-armv7.cmake" ]] || fail "Missing SDK at $sdk. Use --sdk DIR to select the SDK used for the build."

compiler=${CXX:-clang++}
xcode-select -p >/dev/null 2>&1 || fail "Install Apple Command Line Tools with: xcode-select --install"
command -v "$compiler" >/dev/null || fail "Missing Mac C++ compiler: $compiler"
command -v pkg-config >/dev/null && pkg-config --exists libmtp || fail "Install the Mac transfer dependencies with: brew install libmtp pkgconf"

work=$(mktemp -d "${TMPDIR:-/tmp}/manga-viewer-install.XXXXXXXX")
trap 'rm -rf "$work"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
documents="$work/documents"
app="$documents/manga-viewer"
mkdir -p "$app/lib/libheif"
cp -L "$viewer" "$app/manga_kindle"
cp -L "$launcher" "$documents/Manga Viewer.sh"

stage_library() {
    local source=$1 destination=$2 description
    [[ -f "$source" && -r "$source" ]] || fail "Missing SDK runtime file: $source"
    description=$(file -Lb "$source")
    [[ "$description" == *"ELF 32-bit"*"ARM"* ]] || fail "Unexpected SDK runtime file at $source: $description"
    # MTP transfers regular files. Keep the SONAME while following the SDK symlink.
    cp -L "$source" "$app/lib/$destination"
}

for lib in ld-musl-armhf.so.1 libc.musl-armv7.so.1; do
    stage_library "$sdk/sysroot/lib/$lib" "$lib"
done
for lib in libbz2.so.1 libcrypto.so.3 libdav1d.so.7 libde265.so.0 libfbink.so.1 \
        libgcc_s.so.1 libheif.so.1 liblzma.so.5 libpng16.so.16 libsharpyuv.so.0 \
        libstdc++.so.6 libturbojpeg.so.0 libz.so.1 libzip.so.5 libzstd.so.1 \
        libheif/libheif-dav1d.so libheif/libheif-libde265.so; do
    stage_library "$sdk/sysroot/usr/lib/$lib" "$lib"
done

"$compiler" -std=c++17 -O2 -Wall -Wextra -Wpedantic \
    -I"$project_dir/mtp_drive" -isystem "$(pkg-config --variable=includedir libmtp)" \
    "$project_dir/kindle/install-mtp.cpp" "$project_dir/mtp_drive/mtp_device.cpp" \
    -L"$(pkg-config --variable=libdir libmtp)" -lmtp -o "$work/install-mtp"
"$work/install-mtp" "$documents"
echo "Installation verified. Disconnect USB and open Manga Viewer from the Kindle launcher."
