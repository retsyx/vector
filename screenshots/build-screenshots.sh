#!/bin/bash
set -euo pipefail
unset CDPATH

project_dir=$(cd "$(dirname "$0")/.." && pwd -P)
sdk="$HOME/.local/opt/manga-kindle-sdk"
work="$project_dir/build/screenshots"
images="$project_dir/screenshots/images"

usage() {
    cat <<'HELP'
Usage: ./screenshots/build-screenshots.sh [--sdk DIR]

Render the viewer's Kindle screens with sample manga into screenshots/images. The renderer
builds the viewer's drawing code for macOS with the FBInk header from the Kindle SDK. The
default SDK directory is $HOME/.local/opt/manga-kindle-sdk.
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

fbink="$sdk/FBInk-master/fbink.h"
[[ -f "$fbink" ]] || fail "Missing $fbink. Run kindle/build-sdk.sh to install the default SDK, or use --sdk DIR to select an installed SDK."
[[ $(uname -s) == Darwin ]] || fail "Run this script on macOS."
for name in python3 rsvg-convert pkg-config; do
    command -v "$name" >/dev/null || fail "Missing $name. Run: brew install librsvg libheif libzip pkgconf"
done
pkg-config --exists libheif libzip libturbojpeg libpng || fail "Missing libraries. Run: brew install libheif libzip"

rm -rf "$work"
mkdir -p "$work/include/linux" "$images"
python3 -I "$project_dir/screenshots/sample_library.py" "$work"

# FBInk's header only names the Linux framebuffer structs, so declarations stand in for linux/fb.h.
cp "$fbink" "$work/include/"
printf 'struct fb_var_screeninfo;\nstruct fb_fix_screeninfo;\n' >"$work/include/linux/fb.h"

# The viewer's drawing code runs from the page scaling through the page scrubber, before the touch input code.
viewer="$project_dir/viewer_kindle.cpp"
drawing="$work/viewer_drawing.inc"
{
    grep -m1 '^constexpr unsigned fraction_scale' "$viewer"
    awk '/^struct Sample \{/ {on = 1} /^struct Point \{/ {on = 0} on' "$viewer"
    awk '/^std::string reading_status\(/ {on = 1} on {print} on && /^}/ {exit}' "$viewer"
} >"$drawing"
for function in compose_page draw_page draw_reading_dropdown draw_scrubber_preview draw_scrubber_controls reading_status; do
    grep -Eq "^[^[:space:]].* $function\(" "$drawing" || fail "Cannot find $function in viewer_kindle.cpp"
done

# macOS names the stat field st_mtimespec rather than st_mtim.
# shellcheck disable=SC2046
xcrun clang++ -std=c++17 -O2 -Wall -Wextra -Dst_mtim=st_mtimespec \
    -I"$work/include" -I"$work" -I"$project_dir" "$project_dir/screenshots/render.cpp" \
    "$project_dir/kindle_menu.cpp" "$project_dir/kindle_library.cpp" "$project_dir/viewer.cpp" \
    "$project_dir/heif_loader.cpp" $(pkg-config --cflags --libs libheif libzip libturbojpeg libpng) -o "$work/render"

"$work/render" "$work/documents" "$images" "Night Train 01.cbz" 12 47
