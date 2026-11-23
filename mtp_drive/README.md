# MTP Drive for macOS

`mtp_drive` mounts a USB MTP device as a macOS volume. Each storage on the
device appears as a folder. Use Finder or the terminal to browse, copy files in
either direction, and create, rename, move, or delete files and folders as
supported by the device.

## Build

Install the Xcode command-line tools and macFUSE 5.5 or later.
The default mount uses macFUSE's FSKit backend, which requires macOS 15.4 or
later. Enable macFUSE's file system extension in System Settings if prompted.
[macFUSE downloads](https://macfuse.io) · [macFUSE setup instructions](https://github.com/macfuse/macfuse/wiki/Getting-Started)

Build from the repository root.

```sh
brew install cmake pkgconf libmtp
cmake -S mtp_drive -B build-mtp-drive -DCMAKE_BUILD_TYPE=Release
cmake --build build-mtp-drive
```

If `pkg-config` cannot find the installed macFUSE SDK, add
`/usr/local/lib/pkgconfig` to `PKG_CONFIG_PATH` and rerun CMake.

## Mount

Connect and unlock the device, choose USB File Transfer (MTP), and close any
other app using its MTP connection. These commands mount a single connected device.

```sh
./build-mtp-drive/mtp_drive --list
mkdir -p "$HOME/MTP"
./build-mtp-drive/mtp_drive "$HOME/MTP"
```

Use an empty directory you own as the mount point. The driver runs in the
foreground. Open the volume from Finder or run `open "$HOME/MTP"` in another
terminal. With multiple devices, pass `--device BUS:ADDRESS` using an identifier
from `--list`. Use `--read-only` to make every storage read-only, or `--kernel`
to select an already enabled macFUSE kernel backend. The driver also honors
each storage's own write protection.

The driver stages whole files in anonymous temporary files and needs local
disk space for those transfers. Editing or replacing a file also requires
enough free device space for the replacement while the original is still
present. Native moves stay within one storage, and device firmware determines
which rename and move operations succeed. On FSKit, renaming onto an existing
item is refused because macOS can forward a swap request as an ordinary rename.
The volume reports synthetic permissions and ownership. Extended attributes
stay with the source filesystem.

On macOS 27 with FSKit, check the terminal running `mtp_drive` for upload
errors after copying. Apps can report a successful copy when an upload fails
as the file closes. [macFUSE 5.5 release notes](https://github.com/macfuse/macfuse/releases/tag/macfuse-5.5.0)

Finish any transfers and eject the volume in Finder or run `umount "$HOME/MTP"`
before unplugging the device.
