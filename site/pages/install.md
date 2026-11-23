---
title: Install
section: Start
order: 2
description: How to install and set up Shift Engine on Windows, Linux, the Steam Deck, macOS (Apple Silicon) and Android using your own disc files.
---

# Install

Release archives contain only Shift Engine's own programs: `shift-launcher`, `carview`, `carbake`, `soundbake`, `profile-import`, setup scripts and licences. They contain no game data. Verify downloaded archives against `SHA256SUMS.txt` on the [GitHub Releases](https://github.com/arcanite24/pinyon-shift-native/releases/latest) page before running them.

## What you need

- **Your own extracted disc folder** containing `media/` and `default.xex`. Optionally, your Title Update v4 and Xbox 360 DLC packages (STFS containers or extracted offer folders). Shift Engine never downloads game files and never writes to your disc folder.
- **About 8 GB free disk space** for the baked play folder (`bake/`, `sound/`, `colorado.index`).
- **Optional FFmpeg** on `PATH` if you want to bake engine audio loops (`soundbake`) or pack custom radio stations in the launcher.

## Using the launcher

Every desktop release includes `shift-launcher`, a native SDL3 and Vulkan GUI launcher with no Python or Qt dependency:

- **Setup & disc check:** Pick or drag your extracted disc folder onto the **Setup** tab. The launcher checks required archives (`media/tracks/colorado/bin.zip`, `media/db/gamedb.slt` SHA-256, `media/cars/`), checks free disk space, runs `carbake`, runs `soundbake` when FFmpeg is present, writes `play.txt` and builds `colorado.index`.
- **Hardware preset suggestion:** Reads your GPU, VRAM, Vulkan version, CPU threads and display refresh rate to suggest a starting preset (`Performance 120`, `Quality 60`, `Low-Spec 60`, `Balanced 40` or `Ultra Low 30`).
- **Portable Mode:** Place an empty `portable.txt` next to `shift-launcher` (or next to `Shift Engine.app` on macOS), or toggle Portable Mode in the launcher, to keep `play/`, saves, `mods/`, `state/` and `settings.txt` inside a local `data/` directory beside the binary.
- **Content, mods, radio and sync:** Import your own DLC and Title Update v4 packages, toggle and order mods, build custom radio station mods, configure wheels, and sync files to Android over your local Wi-Fi network.

## Windows

1. Download `shift-engine-windows-<version>.zip` and unpack it to a writable folder (for example `D:\ShiftEngine`).
2. Run `shift-launcher.exe`.
3. On the **Setup** tab, choose your extracted disc folder and start setup.
4. Start driving from the **Play** tab, or run `carview.exe` directly.

Command-line alternative from PowerShell:

```powershell
.\setup.ps1 D:\fh1\disc
.\carview.exe
```

Default user data directory: `%APPDATA%\shift-engine\` (or `.\data\` in Portable Mode).

## Linux and Steam Deck

Requires glibc 2.38 or newer (SteamOS 3.7+, Ubuntu 24.04+, Fedora 39+), a Vulkan 1.3 driver (RADV or NVIDIA) and PipeWire, PulseAudio or ALSA.

1. Download `shift-engine-linux-<version>.zip` and unpack it (for example `~/shift-engine`).
2. Run `./shift-launcher` (or `./setup.sh ~/fh1/disc` in a terminal).
3. Complete setup, then use **Add to Steam** in the launcher to create a non-Steam shortcut in Steam's `shortcuts.vdf` (a `.shift-backup` is kept) and a desktop entry.
4. On Steam Deck, switch back to **Gaming Mode** and launch **Shift Engine** under **Non-Steam**. Steam Input maps the built-in controls automatically.

Default user data directory: `~/.local/share/shift-engine/` (or `./data/` in Portable Mode).

## macOS (Apple Silicon)

Requires macOS 13.3 or newer on Apple Silicon. Rendering runs on Vulkan over Metal through MoltenVK (`VK_KHR_portability_enumeration` and `VK_KHR_portability_subset`).

1. Download `shift-engine-macos-<version>.zip`, unpack it and move `Shift Engine.app` to `/Applications` or another writable folder.
2. Release builds are not Apple-notarized. On first launch, approve the app once in **System Settings → Privacy & Security → Open Anyway**.
3. Launch `Shift Engine.app`. The bundled `libMoltenVK.dylib` inside `Shift Engine.app/Contents/Frameworks` is loaded automatically (Homebrew `/opt/homebrew/lib` is also checked when running locally built binaries).
4. Complete setup and launch Colorado.

### macOS keyboard shortcuts

| Action | Shortcut |
| --- | --- |
| Live settings overlay | <kbd>Cmd</kbd> + <kbd>,</kbd> |
| Toggle fullscreen | <kbd>Ctrl</kbd> + <kbd>Cmd</kbd> + <kbd>F</kbd> |
| Photo Mode | <kbd>Cmd</kbd> + <kbd>Shift</kbd> + <kbd>P</kbd> |
| Step time of day / Trainer | <kbd>Cmd</kbd> + <kbd>T</kbd> |
| Quit | <kbd>Cmd</kbd> + <kbd>Q</kbd> |

Default user data directory: `~/Library/Application Support/shift-engine/` (or `data/` beside `Shift Engine.app` when `portable.txt` is present).

## Android

Supports arm64 Android 10+ (API 29+) phones, tablets and handhelds with Vulkan 1.1+.

### Install and LAN Wi-Fi Sync

1. Run setup once on your PC in `shift-launcher`.
2. Open the **Android** tab in `shift-launcher` and start the Wi-Fi Sync server. Select which folders to transfer: baked `play/`, DLC and Title Update v4, enabled mods, and custom radio stations.
3. Install `shift-drive-arm64.apk` either by scanning the QR code on your local network or over USB with `adb install shift-drive-arm64.apk`.
4. Open **Shift Drive** on Android and enter the 6-digit pairing code shown in the launcher. Transfers run directly over your private LAN into `Android/data/net.shiftengine.drive/files/play` using resumable HTTP Range requests. Nothing is sent over the internet.

### Touch controls, presets and Turnip drivers

- **Touch HUD:** Appears on first touch and hides automatically when a Bluetooth, USB or built-in gamepad is used (or after 20 seconds without touch). Supports left steering buttons, floating virtual thumbstick or tilt steering, analog gas/brake/handbrake pads, shift paddles and quick-action chips for rewind, camera, radio, map and pause.
- **Presets:** `smooth-60` (2x MSAA, shadows off, quarter-rate reflections, 60 fps target), `quality-30` (4x MSAA, low shadows, bloom on, 30 fps target) and `low-power` (1x MSAA, shadows and bloom off, low draw distance).
- **Mesa Turnip (Adreno 7xx/8xx):** Place a Turnip driver folder containing `libvulkan_freedreno.so` in `Android/data/net.shiftengine.drive/files/state/drivers/turnip/` and set `android-gpu-driver auto` or the folder name in `settings.txt`.

## Controls

| Action | Keyboard | Gamepad | Touch HUD |
| --- | --- | --- | --- |
| Throttle | <kbd>W</kbd> / <kbd>Up</kbd> | Right Trigger | `GAS` |
| Brake / Reverse | <kbd>S</kbd> / <kbd>Down</kbd> | Left Trigger | `BRK` |
| Steer | <kbd>A</kbd> / <kbd>D</kbd> or <kbd>Left</kbd> / <kbd>Right</kbd> | Left Stick | `<` / `>` / Stick / Tilt |
| Handbrake | <kbd>Space</kbd> | `A` | `HB` |
| Shift Up / Down | <kbd>E</kbd> / <kbd>Q</kbd> | `B` / `X` | `+` / `-` |
| Rewind | <kbd>Backspace</kbd> / <kbd>Y</kbd> | `Y` | `RW` |
| Camera | <kbd>V</kbd> | `RB` | `CAM` |
| Radio Station | <kbd>,</kbd> / <kbd>.</kbd> | D-Pad Left / Right | `RAD` |
| World Map | <kbd>M</kbd> | `Back` | `MAP` |
| Pause Menu | <kbd>Esc</kbd> | `Start` | `II` |
| Live Settings | <kbd>F6</kbd> (<kbd>Cmd</kbd>+<kbd>,</kbd> on macOS) | Pause → Settings | Pause → Settings |
| Achievements | <kbd>F7</kbd> | Pause → Achievements | Pause → Achievements |
| Screenshot PNG | <kbd>F8</kbd> | — | — |
| Photo Mode | <kbd>P</kbd> (<kbd>Cmd</kbd>+<kbd>Shift</kbd>+<kbd>P</kbd> on macOS) | Pause → Photo Mode | Pause → Photo Mode |
| Trainer | <kbd>F10</kbd> | Pause → Trainer | Pause → Trainer |

## Troubleshooting

- **"no play folder found":** Run setup in `shift-launcher`, set `SHIFT_PLAY_DIR`, or pass `--play <dir>` to `carview`.
- **Low frame rate or Vulkan startup error:** Update your GPU driver and apply the preset suggested on the launcher's Play tab, or pass `--preset low-power`.
- **Android Wi-Fi Sync cannot reach the PC:** Confirm both devices are on the same private LAN and permit local TCP port `47615` and UDP port `47616` in your firewall.
- **Persistent settings:** Settings live in `settings.txt` in your user data directory (`data/settings.txt` in Portable Mode). Command-line flags override `settings.txt`; pass `--save-settings` to write current flags back.
