# Vector Shell

Vector Shell runs shell commands as root on a jailbroken Kindle over USB. It has two parts:

- `kindle/Vector Shell.sh`, a scriptlet that starts a small server on the Kindle.
- `kindle/kshell.cpp`, a Mac tool that sends commands to the server over MTP.

While the server runs, anything that can write to the Kindle's storage can run commands as root on it. Stop it when you are done.

## Build

Install the Command Line Tools and the MTP library with `brew install libmtp pkgconf`. In the vector directory run:

```sh
mkdir -p build
clang++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Imtp_drive \
    -isystem "$(pkg-config --variable=includedir libmtp)" \
    kindle/kshell.cpp mtp_drive/mtp_device.cpp \
    -L"$(pkg-config --variable=libdir libmtp)" -lmtp -o build/kshell
```

## Install

Connect the Kindle in USB file-transfer mode, close other MTP apps, and copy the scriptlet to the Kindle:

```sh
build/kshell put "kindle/Vector Shell.sh" /documents
```

## Start

The Kindle shows a "Connected to computer" screen while USB is connected. Disconnect USB, tap **Vector Shell** in the Library, and reconnect. Tapping it again restarts the server.

## Run commands

```sh
build/kshell 'id; cat /etc/prettyversion.txt'
build/kshell < script.sh
```

`kshell` prints the output and exits with the command's status. `-t SECONDS` sets the timeout (default 120) and `-o BYTES` the output capacity (default 256 KiB). Commands run as root from `/`.

While USB is connected, the Kindle blocks its own access to `/mnt/us`. Use `/mnt/base-us`, the partition behind it, instead. Commands get the exchange folder as `$VECTOR_SHELL`.

## Transfer files

```sh
build/kshell pull /var/local/appreg.db appreg.db   # any file on the Kindle
build/kshell get /documents/book.azw3 book.azw3     # a file in user storage
build/kshell put book.azw3 /documents               # replaces a file with the same name
build/kshell ls /documents
```

## Stop

```sh
build/kshell stop
```

Stop the shell before disconnecting USB. The server also stops after an hour without commands and at reboot. It keeps the screensaver off while it runs and logs to `vector-shell/out/server.log` in user storage. `build/kshell clean` deletes leftover exchange files.

To remove Vector Shell, stop it and delete `documents/Vector Shell.sh` and the `vector-shell` folder from the Kindle.

## How it works

`/mnt/us` on the Kindle is an `fsp` FUSE view of `/mnt/base-us`, and it blocks while USB is connected, so the server works in `/mnt/base-us/vector-shell`. The MTP server may not show files that the Kindle creates or deletes during a USB session, so `kshell` creates every exchange file itself and deletes it afterward:

1. `kshell` uploads `in/NAME.sh`, placeholders `out/NAME.out` and `out/NAME.rc`, and then `in/NAME.go`.
2. The server polls `in/` once a second. For each new `NAME.go` it runs `NAME.sh`, writes the output over `out/NAME.out`, and writes `STATUS LENGTH` over `out/NAME.rc`.
3. `kshell` polls `out/NAME.rc`, reads the output, and deletes all four files.

`pull` copies a file the same way, through a placeholder of the file's size.
