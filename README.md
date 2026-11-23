# Vector
The Kindle Manga Vector

## Prerequisites

In the vector directory run:

```sh
brew install cmake ninja pkgconf

./kindle/build-sdk.sh
```

This will build the Kindle SDK and place it by default in `$HOME/.local/opt/manga-kindle-sdk`. Use the `--install` option to specify a different SDK installation path.

## Build

In the vector directory run:

```sh
./kindle/build-viewer.sh
```

Use the `--sdk` option to use a custom SDK installation path that you specified with the `--install` option in `build-sdk.sh`. For example:

```sh
./kindle/build-sdk.sh --install "$HOME/SDKs/manga-kindle"
./kindle/build-viewer.sh --sdk "$HOME/SDKs/manga-kindle"
```

## Install

Connect the USB and make sure it is in transfer mode.

Run:

```sh
./kindle/install-viewer.sh
```

If you built with a custom SDK, pass the same path. For example:

```sh
./kindle/install-viewer.sh --sdk "$HOME/SDKs/manga-kindle"
```