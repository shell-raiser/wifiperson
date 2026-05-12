# WiFiperson

WiFiperson is a Qt 6 desktop app that scans nearby Wi-Fi networks and visualizes channel usage.

## Requirements

Linux:

- CMake 3.16 or newer
- A C++17 compiler
- Qt 6 Core, Widgets, Charts, and Network development packages
- libnl 3 and libnl-genl development packages
- pkg-config

Windows:

- CMake 3.16 or newer
- MSVC with a C++17 toolchain
- Qt 6 Core, Widgets, Charts, and Network development packages
- Windows WLAN API, provided by the Windows SDK

## Build

```sh
cmake --preset release
cmake --build --preset release
```

The local build directory is `build/`.

To install into a staging directory:

```sh
DESTDIR="$PWD/package-root" cmake --install build
```

GitHub Actions builds Linux packages and Windows packages on every push to `main`. The Windows artifact contains a single Qt Installer Framework EXE instead of a zipped deployed folder. Each successful run publishes the build files to a GitHub release tagged as `build-<commit-sha>`.
