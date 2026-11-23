#!/bin/bash
set -euo pipefail
unset CDPATH

project_dir=$(cd "$(dirname "$0")/.." && pwd -P)
assets="$project_dir/kindle/sdk"
output="$project_dir/build/kindle-sdk"
cache=
install_dir="$HOME/.local/opt/manga-kindle-sdk"

usage() {
    cat <<'HELP'
Usage: ./kindle/build-sdk.sh [--output DIR] [--cache DIR] [--install DIR]

Build and package the Kindle SDK on Apple Silicon macOS.
The SDK installs into $HOME/.local/opt/manga-kindle-sdk by default.
Use --install DIR to select a different new directory.
The archive goes in build/kindle-sdk by default. Downloads are SHA-256 verified
and cached in its downloads/ directory for offline rebuilds.
HELP
}

fail() { echo "$*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --output|--cache|--install)
            [[ -n ${2:-} && $2 != --* ]] || fail "$1 needs a directory"
            case "$1" in
                --output) output=$2 ;;
                --cache) cache=$2 ;;
                --install) install_dir=$2 ;;
            esac
            shift 2 ;;
        *) usage >&2; fail "Unknown argument: $1" ;;
    esac
done

[[ $(uname -s) == Darwin && $(uname -m) == arm64 ]] || fail "Run this script in a native Apple Silicon macOS terminal."
xcode-select -p >/dev/null 2>&1 || fail "Install the Apple Command Line Tools with: xcode-select --install"

[[ ! -e "$install_dir" && ! -L "$install_dir" ]] || fail "Install destination already exists: $install_dir. To build with an installed SDK, run ./kindle/build-viewer.sh --sdk \"$install_dir\". To install another SDK, use --install with a new directory."
mkdir -p "$(dirname "$install_dir")"
install_parent=$(cd "$(dirname "$install_dir")" && pwd -P)
install_name=$(basename "$install_dir")
cache=${cache:-"$output/downloads"}
mkdir -p "$output" "$cache"
output=$(cd "$output" && pwd -P)
cache=$(cd "$cache" && pwd -P)
archive=manga-viewer-sdk-macos-arm64.tar.gz
for artifact in "$archive" fbink.log; do
    [[ ! -d "$output/$artifact" ]] || fail "Expected a file at $output/$artifact"
done
work=$(mktemp -d "$output/.sdk-build.XXXXXXXX")
fetch_tmp=
install_tmp=
cleanup() {
    rm -rf "$work"
    if [[ -n "$fetch_tmp" ]]; then rm -rf "$fetch_tmp"; fi
    if [[ -n "$install_tmp" ]]; then rm -rf "$install_tmp"; fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
fetch_tmp=$(mktemp -d "$cache/.fetch.XXXXXXXX")

matches_hash() {
    local digest
    digest=$(shasum -a 256 < "$1") || return 1
    [[ ${digest%% *} == "$2" ]]
}

sdk="$work/manga-kindle-sdk"
fbink="$sdk/FBInk-master"
mkdir -p "$fbink" "$sdk/sysroot"
while read -r hash url; do
    name=${url##*/}
    input="$cache/$name"
    if [[ ! -f "$input" ]] || ! matches_hash "$input" "$hash"; then
        echo "Downloading $name"
        if ! curl --fail --silent --show-error --location --retry 3 \
                --proto '=https' --proto-redir '=https' -o "$fetch_tmp/$name" "$url"; then
            fail "Download failed: $url. A cached copy named $name can be supplied with --cache."
        fi
        matches_hash "$fetch_tmp/$name" "$hash" || fail "SHA-256 mismatch for $url"
        mv -f "$fetch_tmp/$name" "$input"
    fi
    case "$url" in
        https://ziglang.org/*) /usr/bin/tar -xf "$input" -C "$sdk" ;;
        https://codeload.github.com/NiLuJe/FBInk/*)
            /usr/bin/tar -xf "$input" --strip-components 1 -C "$fbink"
            fbink_revision=$name ;;
        */stb_image.h) mkdir -p "$fbink/stb"; cp "$input" "$fbink/stb/stb_image.h" ;;
        *.apk) /usr/bin/tar -xf "$input" --exclude '.*' -C "$sdk/sysroot" ;;
        *) fail "Unknown SDK source: $url" ;;
    esac
done < "$assets/sources.lock"

install -m 755 "$assets/arm-tool" "$sdk/arm-tool"
cp "$assets/kindle-armv7.cmake" "$assets/sources.lock" "$sdk/"
for tool in cc cxx ar ranlib; do ln -s arm-tool "$sdk/arm-$tool"; done

echo "Building Kindle FBInk"
# The upstream shared-library rule also depends on a Kobo-only I2C build.
if ! PATH="$sdk:$PATH" make -C "$fbink" -o libi2c.built -j4 CC=arm-cc \
        DEBUG= KINDLE=1 MINIMAL=1 IMAGE=1 INPUT=1 FBINK_VERSION="$fbink_revision" \
        sharedlib >"$work/fbink.log" 2>&1; then
    cat "$work/fbink.log" >&2
    fail "FBInk build failed"
fi
ln -s ../../../FBInk-master/fbink.h "$sdk/sysroot/usr/include/fbink.h"
for lib in "$fbink"/Release/libfbink.so*; do
    name=${lib##*/}
    ln -s "../../../FBInk-master/Release/$name" "$sdk/sysroot/usr/lib/$name"
done

echo "Packaging $archive"
COPYFILE_DISABLE=1 /usr/bin/tar --no-mac-metadata --no-xattrs \
    --exclude 'manga-kindle-sdk/FBInk-master/Release/shared' \
    --exclude 'manga-kindle-sdk/FBInk-master/Release/static' \
    -czf "$work/$archive" -C "$work" manga-kindle-sdk

install_tmp=$(mktemp -d "$install_parent/.manga-sdk-install.XXXXXXXX")
mkdir "$install_tmp/$install_name"
/usr/bin/tar -xf "$work/$archive" --strip-components 1 -C "$install_tmp/$install_name"

mv -f "$work/fbink.log" "$output/fbink.log"
mv -f "$work/$archive" "$output/$archive"
mv -n "$install_tmp/$install_name" "$install_parent/"
[[ ! -d "$install_tmp/$install_name" ]] || fail "Install destination appeared during the build: $install_dir"
echo "Installed SDK: $install_parent/$install_name"
echo "SDK archive: $output/$archive"
