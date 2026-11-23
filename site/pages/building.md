---
title: Building
section: Technical
order: 2
description: How to build Shift Engine from source on Windows, Linux, macOS (Apple Silicon) and Android, run tests, regenerate shaders and package releases.
---

# Building from source

Shift Engine uses CMake 3.25+, Ninja and C++20. Third-party dependencies (`libmspack`, `SDL3`, `Vulkan-Headers`, `volk`, `VulkanMemoryAllocator`, `SQLite`, `stb`, `dr_libs`) are fetched and pinned automatically at configure time by `cmake/Dependencies.cmake`.

You do **not** need a Vulkan SDK installed to compile: headers and `volk` are fetched automatically, and prebuilt SPIR-V shaders live under `apps/carview/shaders/prebuilt/`.

## Windows (MSVC)

Requires Visual Studio 2022 (or Build Tools with the C++ x64 workload), CMake 3.25+, Ninja and Git. From any PowerShell prompt:

```powershell
.\tools\build-windows.ps1 -BuildDir build -Config Release
ctest --test-dir build -LE corpus --output-on-failure -j 4
```

`tools/build-windows.ps1` locates Visual Studio via `vswhere.exe` and enters the x64 developer environment automatically.

## Linux

Requires GCC 13+ or Clang 17+, CMake 3.25+, Ninja, Git and SDL3's X11/Wayland/audio development headers. On Ubuntu 24.04 or Debian:

```sh
sudo apt-get install -y \
  cmake ninja-build build-essential pkg-config \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev \
  libxi-dev libxfixes-dev libxss-dev \
  libwayland-dev libxkbcommon-dev \
  libasound2-dev libpulse-dev libdbus-1-dev libudev-dev

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j "$(nproc)"
ctest --test-dir build -LE corpus --output-on-failure -j "$(nproc)"
```

`SHIFT_STATIC_RUNTIME=ON` (default on Linux) statically links `libstdc++` and `libgcc` for portable binaries across glibc 2.38+ distributions and SteamOS.

## macOS (Apple Silicon)

Requires Xcode Command Line Tools (`xcode-select --install`), Homebrew, CMake, Ninja and MoltenVK:

```sh
brew install cmake ninja molten-vk
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j "$(sysctl -n hw.ncpu)"
ctest --test-dir build -LE corpus --output-on-failure -j "$(sysctl -n hw.ncpu)"
```

To package a self-contained `Shift Engine.app` bundle with `libMoltenVK.dylib` inside `Contents/Frameworks`:

```sh
python3 tools/package.py --build build --platform macos \
  --moltenvk "$(brew --prefix molten-vk)/lib/libMoltenVK.dylib"
```

## Android (arm64 APK)

Requires the Android SDK (platform `android-35` and matching `build-tools`), Android NDK (r27+), JDK 11+, CMake 3.25+ and Ninja. No Gradle wrapper is needed; `tools/android-apk.py` compiles SDL's and Shift Engine's Java glue with `javac`/`d8`, links with `aapt2`, aligns with `zipalign` and signs with `apksigner`.

```sh
cmake -S . -B build-android -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-29 \
  -DSHIFT_BUILD_LAUNCHER=OFF
cmake --build build-android --target carview -j "$(nproc)"
python3 tools/android-apk.py --build build-android --out out/release
```

Pass `-DSHIFT_ANDROID_EXECUTABLE=ON` instead if you want a standalone `carview` binary to run directly over `adb shell` for benchmarking.

## CMake options

| Option | Default | Effect |
| --- | --- | --- |
| `SHIFT_BUILD_CARVIEW` | `ON` | Build the SDL3 + Vulkan runtime (`carview`) |
| `SHIFT_BUILD_LAUNCHER` | `ON` | Build `shift-launcher` (automatically skipped on Android) |
| `SHIFT_STATIC_RUNTIME` | `ON` | Link `libstdc++` and `libgcc` statically on Linux |
| `SHIFT_GLSL_COMPILER` | auto-detect | Explicit path to `glslc` or `glslangValidator` |
| `SHIFT_ANDROID_EXECUTABLE` | `OFF` | Build `carview` as a CLI binary instead of `libmain.so` on Android |

## Shaders

When `glslc` or `glslangValidator` is on `PATH`, inside `$VULKAN_SDK`, or passed via `-DSHIFT_GLSL_COMPILER`, CMake compiles shaders directly from `apps/carview/shaders/`. Otherwise it embeds the prebuilt SPIR-V in `apps/carview/shaders/prebuilt/`.

After editing any `.vert`, `.frag`, `.comp` or `.glsl` file, refresh the prebuilt SPIR-V binaries before committing:

```sh
python tools/regenerate-shaders.py [--compiler /path/to/glslc]
```

## Tests and packaging

- **Unit tests (no game files required):** `ctest --test-dir build -LE corpus --output-on-failure` runs every synthetic-byte test suite.
- **Corpus tests (optional, uses your local disc):** Set `SHIFT_GAME_ROOT` (and optionally `SHIFT_STATE_ROOT`) and run `ctest --test-dir build -L corpus --output-on-failure`.
- **Clean-room and tool checks:**

```sh
python tools/check-provenance.py
python tools/benchmark.py --self-test
python tools/android-apk.py --validate-only
python site/build.py
```

- **Packaging a release archive:**

```sh
python tools/package.py --build build --platform windows --out out/release
```

Pass `--check --game-root <disc> [--state-root <state>]` to run the [binary-content fingerprint check](legal.md#verification-checks) against your local game files before publishing.
