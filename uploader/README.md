# Kindle Uploader
Copy files to your Kindle over USB.

## Prerequisites

Install the Xcode command-line tools and [Homebrew](https://brew.sh). Then run:

```sh
brew install libmtp
```

## Build

In the vector directory run:

```sh
./uploader/build-app.sh
```

This will build `Kindle Uploader.app` in the uploader directory. The app includes its own copies of libmtp and libusb, so you can move it anywhere, such as `/Applications`.
