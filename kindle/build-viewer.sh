#!/bin/bash
set -euo pipefail
unset CDPATH

project_dir=$(cd "$(dirname "$0")/.." && pwd -P)
sdk="$HOME/.local/opt/manga-kindle-sdk"
output="$project_dir/build/kindle-viewer"

usage() {
    cat <<'HELP'
Usage: ./kindle/build-viewer.sh [--sdk DIR]

Build Manga Viewer using the installed Kindle SDK. The default SDK directory is
$HOME/.local/opt/manga-kindle-sdk. The executable is build/kindle-viewer/manga_kindle.
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

[[ -f "$sdk/kindle-armv7.cmake" ]] || fail "Missing $sdk/kindle-armv7.cmake. Run kindle/build-sdk.sh to install the default SDK, or use --sdk DIR to select an installed SDK."
sdk=$(cd "$sdk" && pwd -P)
[[ $(uname -s) == Darwin && $(uname -m) == arm64 ]] || fail "Run this script in a native Apple Silicon macOS terminal."
for name in cmake ninja pkg-config; do
    if ! command -v "$name" >/dev/null; then
        command -v brew >/dev/null || fail "Install Homebrew, then run: brew install cmake ninja pkgconf"
        formula=$name
        [[ "$name" != pkg-config ]] || formula=pkgconf
        brew install "$formula"
    fi
done

# CMake caches absolute compiler and library paths in the build directory.
sdk_hash=$(printf '%s' "$sdk" | shasum -a 256)
build="$output/cmake/${sdk_hash%% *}"
binary="$output/manga_kindle"
[[ ! -d "$binary" ]] || fail "Expected a file at $binary"
cmake -S "$project_dir" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$sdk/kindle-armv7.cmake" \
    -DCMAKE_CXX_FLAGS=-fPIC -DMANGA_BUILD_KINDLE_VIEWER=ON
cmake --build "$build" --target manga_kindle --parallel

description=$(file -b "$build/manga_kindle")
[[ "$description" == *"ELF 32-bit"*"ARM"*"/lib/ld-musl-armhf.so.1"* ]] || fail "Unexpected viewer binary: $description"
staged=$(mktemp "$output/.manga_kindle.XXXXXXXX")
trap 'rm -f "$staged"' EXIT
install -m 755 "$build/manga_kindle" "$staged"
mv -f "$staged" "$binary"
echo "Kindle viewer: $binary"
