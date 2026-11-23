#!/bin/bash
set -euo pipefail

cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
mtp="$(brew --prefix libmtp)"
usb="$(brew --prefix libusb)"
mtp_library="$mtp/lib/libmtp.9.dylib"
usb_library="$usb/lib/libusb-1.0.0.dylib"

# The bundle must require at least the deployment target of either dependency.
minimum_macos="$(
    for library in "$mtp_library" "$usb_library"; do
        xcrun vtool -show-build "$library" | awk '/minos/ {print $2}'
    done | sort -V | tail -1
)"
staging="$(mktemp -d "${TMPDIR:-/tmp}/kindle-app.XXXXXXXX")"
trap 'rm -rf -- "$staging"' EXIT
app="$staging/Kindle Uploader.app"
mkdir -p "$app/Contents/"{MacOS,Frameworks,Resources}
cp Info.plist "$app/Contents/"

iconset="$staging/AppIcon.iconset"
mkdir "$iconset"
for size in 16 32 128 256 512; do
    /usr/bin/sips -Z "$size" AppIcon.png --out "$iconset/icon_${size}x${size}.png" >/dev/null
    /usr/bin/sips -Z "$((size * 2))" AppIcon.png --out "$iconset/icon_${size}x${size}@2x.png" >/dev/null
done
/usr/bin/iconutil -c icns "$iconset" -o "$app/Contents/Resources/AppIcon.icns"
/usr/libexec/PlistBuddy -c "Add :LSMinimumSystemVersion string $minimum_macos" "$app/Contents/Info.plist"

target="$(uname -m)-apple-macos$minimum_macos"
xcrun swiftc -target "$target" -module-cache-path "$staging/ModuleCache" -parse-as-library -O KindleUploader.swift \
    -o "$app/Contents/MacOS/KindleUploader"
xcrun clang++ -target "$target" -std=c++17 -O2 -Wall -Wextra -I"$mtp/include" \
    transfer.cpp "$mtp_library" -o "$app/Contents/MacOS/kindle-transfer"

cp -L "$mtp_library" "$app/Contents/Frameworks/libmtp.9.dylib"
cp -L "$usb_library" "$app/Contents/Frameworks/libusb-1.0.0.dylib"
cp "$mtp/COPYING" "$app/Contents/Resources/libmtp-COPYING.txt"
cp "$usb/COPYING" "$app/Contents/Resources/libusb-COPYING.txt"
chmod u+w "$app"/Contents/Frameworks/*.dylib

mtp_id="$(xcrun otool -D "$mtp_library" | sed -n '2p')"
usb_id="$(xcrun otool -D "$usb_library" | sed -n '2p')"
xcrun install_name_tool -id '@rpath/libusb-1.0.0.dylib' \
    "$app/Contents/Frameworks/libusb-1.0.0.dylib"
xcrun install_name_tool -id '@rpath/libmtp.9.dylib' \
    -change "$usb_id" '@loader_path/libusb-1.0.0.dylib' \
    "$app/Contents/Frameworks/libmtp.9.dylib"
xcrun install_name_tool -change "$mtp_id" '@executable_path/../Frameworks/libmtp.9.dylib' \
    "$app/Contents/MacOS/kindle-transfer"

/usr/bin/codesign --force --sign - "$app"/Contents/Frameworks/*.dylib "$app/Contents/MacOS/kindle-transfer"
/usr/bin/codesign --force --sign - "$app"
/usr/bin/codesign --verify --deep --strict "$app"

output="$PWD/Kindle Uploader.app"
rm -rf -- "$output"
/usr/bin/ditto "$app" "$output"
printf '%s\n' "$output"
