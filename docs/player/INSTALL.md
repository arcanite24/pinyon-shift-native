# Installing shift-engine

`shift-engine` is a native, cross-platform engine that plays Forza Horizon (2012)
from your own copy of the game files on **Windows**, **Linux / Steam Deck**,
**macOS**, and **Android**. Release packages contain only the engine's own
programs — zero game files and zero game code.

## What you need

- **Your own game files.** The disc's contents extracted to a folder holding
  `media/` and `default.xex`. Optionally, your Title Update v4, DLC STFS/offer
  packages, UDLC v1.2, or Forza Horizon XE folders. `shift-engine` never
  downloads game files and cannot play without yours.
- **A GPU with Vulkan 1.3** on desktop (or Vulkan 1.1+ / MoltenVK on macOS and
  Android). Tested on Intel UHD 620, AMD Van Gogh (Steam Deck), NVIDIA/AMD
  desktop GPUs, Apple Silicon (MoltenVK), and Snapdragon/Adreno (Turnip).
- **About 8 GB free** for the baked play folder.
- **Optional: FFmpeg** on PATH for offline engine sound baking and the
  Launcher's Custom Radio Station Builder.

## Unified GUI Launcher (Recommended — Zero Python/Qt Required)

Every desktop release bundles **`shift-launcher`**, a native SDL3 + Vulkan GUI
launcher with no Python or Qt runtime dependency:

- **One-Click Setup & Disc Verification:** Drag and drop your extracted disc
  folder onto the launcher (or pick it on the **SETUP** tab). The launcher
  verifies the required files (`media/tracks/colorado/bin.zip`,
  `media/db/gamedb.slt` SHA-256, `media/cars/`), checks free disk space, runs
  `carbake`, optional `soundbake`, writes `play.txt`, and builds
  `colorado.index` with a live progress bar and log drawer.
- **Play & Hardware Advisor ("This PC"):** Detects your GPU, VRAM, Vulkan
  version, CPU cores/threads, and display refresh rate, and recommends the
  best preset (`Performance 120`, `Quality 60`, `Low-Spec 60`, `Balanced 40`,
  or `Ultra Low 30`).
- **Portable Mode:** Drop an empty `portable.txt` next to `shift-launcher`
  (or next to `Shift Engine.app` on macOS, or toggle **PORTABLE** on the Play
  tab) to keep `play/`, `drive/` saves, `mods/`, `state/`, and `settings.txt`
  in a self-contained `data/` folder beside the launcher.
- **Add to Steam:** Writes a non-Steam shortcut into Steam's `shortcuts.vdf`
  (keeping a `.shift-backup`) and a Linux desktop entry for Steam Deck Gaming
  Mode.
- **DLC, Title Update v4, UDLC v1.2 & Forza Horizon XE:** Verify and import
  player-owned STFS containers (`CON`/`LIVE`/`PIRS`) or extracted folders.
- **Mod Manager, Custom Radio Station Builder, Wheel Configurator, Self-Updater,
  and Android Wi-Fi Sync:** All built directly into the launcher tabs.

---

## Windows

1. Unzip `shift-engine-windows-<version>.zip` to a writable folder (e.g.
   `D:\ShiftEngine`).
2. Run **`shift-launcher.exe`** (or run `.\setup.ps1 D:\fh1\disc` from
   PowerShell if you prefer the CLI).
3. On the **SETUP** tab, drop your extracted disc folder and click **RUN SETUP**.
4. Click **PLAY COLORADO NOW** (or run `carview.exe` directly).

Default data folder: `%APPDATA%\shift-engine\` (or `.\data\` in Portable Mode).

## Linux and Steam Deck

Requires glibc 2.38+ (SteamOS 3.7+, Ubuntu 24.04, Fedora 39+) and ALSA/PipeWire/PulseAudio.

1. Unpack `shift-engine-linux-<version>.zip` (e.g. `~/shift-engine`).
2. Run **`./shift-launcher`** (or `./setup.sh ~/fh1/disc` from a terminal).
3. Complete **SETUP**, then click **ADD TO STEAM** on the **PLAY** tab.
4. On Steam Deck, return to **Gaming Mode** and launch **Shift Engine** from
   **Non-Steam** — Steam Input maps the built-in controls automatically.

Default data folder: `~/.local/share/shift-engine/` (or `./data/` in Portable Mode).

## macOS (Apple Silicon & Intel)

Runs natively on macOS 13.3+ using Vulkan over Metal via **MoltenVK**
(`VK_KHR_portability_enumeration` and `VK_KHR_portability_subset`).

1. Unzip `shift-engine-macos-<version>.zip` and move **`Shift Engine.app`** to
   `/Applications` (or any writable folder).
2. On first launch, approve the unsigned bundle in **System Settings -> Privacy
   & Security -> Open Anyway**.
3. Open **`Shift Engine.app`** (bundled `libMoltenVK.dylib` is discovered
   automatically inside `Shift Engine.app/Contents/Frameworks`, or from
   Homebrew `/opt/homebrew/lib`).
4. Complete **SETUP** and click **PLAY COLORADO NOW**.

### macOS Keyboard Shortcuts

| Action | Shortcut |
| --- | --- |
| Open Settings Overlay | `Cmd + ,` |
| Toggle Fullscreen | `Ctrl + Cmd + F` |
| Open Photo Mode | `Cmd + Shift + P` |
| Bug Bookmark (Playtest Mode) | `Cmd + Shift + B` |
| Step Time of Day / Trainer | `Cmd + T` |
| Quit | `Cmd + Q` |

Default data folder: `~/Library/Application Support/shift-engine/` (or place
`portable.txt` beside `Shift Engine.app` for a self-contained `data/` folder).

## Android (Phones, Tablets & Handhelds — e.g. Odin 2 Portal)

Supports arm64 Android 10+ (API 29+) devices with Vulkan 1.1+.

### Install & Wi-Fi Sync (No USB / adb Required)

1. Run setup once on your PC in **`shift-launcher`**.
2. Open the **ANDROID** tab in `shift-launcher` and click **START WI-FI SYNC SERVER**.
   Choose which groups to share: **Baked Play Folder**, **DLC & Title Update v4**,
   **Enabled Mods**, and **Custom Radio Stations**.
3. Scan the **QR code** with your Android device to download and install
   `shift-drive-arm64.apk`, or install via USB with `adb install shift-drive-arm64.apk`.
4. Open **Shift Drive** on Android and enter the **6-digit pairing code** shown
   in the launcher. Files sync directly over your private LAN into
   `Android/data/net.shiftengine.drive/files/play` with resumable HTTP Range
   downloads (nothing is ever uploaded to the internet).

### Android Touch Controls & Presets

- **Multi-Touch Driving HUD:** Appears automatically at the first touch and
  **auto-hides** as soon as a physical Bluetooth/USB/built-in gamepad is used
  (or after 20 seconds without touch). Includes:
  - Left steering **Buttons (`<` / `>`)**, floating **Virtual Thumbstick**, or
    **Tilt** steering (switchable anytime via the top `BTN/STICK/TILT` chip).
  - Right analog **Gas**, **Brake**, and **Handbrake** pedals plus **Shift `+`/`-`**
    paddles.
  - Top quick-action bar: **Rewind (`RW`)**, **Camera (`CAM`)**, **Radio (`RAD`)**,
    **Map (`MAP`)**, and **Pause (`II`)**.
  - Scaled safely for 20:9 phones, 16:9 handhelds (Odin), and 4:3/16:10 tablets.
- **Android Graphics Presets (`--preset` or `settings.txt`):**
  - `smooth-60`: 2x MSAA, shadows off, quarter-rate reflections, 60 fps target.
  - `quality-30`: 4x MSAA, low shadows, bloom on, quarter-rate reflections, 30 fps target.
  - `low-power`: 1x MSAA, shadows off, bloom off, low draw distance, 30 fps target.
- **Custom Turnip Vulkan Driver (Adreno 7xx/8xx):** Place a Mesa Turnip driver
  folder (`libvulkan_freedreno.so`) under
  `Android/data/net.shiftengine.drive/files/state/drivers/turnip/` (or pass
  `android-gpu-driver auto|system|<folder>` in `settings.txt`).

---

## Controls

| Action | Keyboard | Gamepad | Touch HUD |
| --- | --- | --- | --- |
| Throttle | `W` / `Up` | Right Trigger | `GAS` pad |
| Brake / Reverse | `S` / `Down` | Left Trigger | `BRK` pad |
| Steer | `A`/`D` or `Left`/`Right` | Left Stick | `<`/`>` or Stick/Tilt |
| Handbrake | `Space` | `A` | `HB` pad |
| Shift Up / Down | `E` / `Q` | `B` / `X` | `+` / `-` paddles |
| Rewind | `Backspace` / `Y` | `Y` | `RW` chip |
| Camera | `V` | `RB` | `CAM` chip |
| Radio Station | `,` / `.` | D-Pad Left/Right | `RAD` chip |
| World Map | `M` | `Back` | `MAP` chip |
| Pause Menu | `Esc` | `Start` | `II` chip |
| Live Settings | `F6` (`Cmd+,` on macOS) | Pause -> Settings | Pause -> Settings |
| Achievements | `F7` | Pause -> Achievements | Pause -> Achievements |
| Photo Mode | `P` (`Cmd+Shift+P` on macOS) | Pause -> Photo Mode | Pause -> Photo Mode |
| Screenshot PNG | `F8` | — | — |

## Settings & Troubleshooting

Settings are stored in `settings.txt` in your user folder (or `data/settings.txt`
in Portable Mode). Command-line flags override `settings.txt`, and
`--save-settings` persists your current options.

- **"no play folder found"**: run **SETUP** in `shift-launcher` or pass `--play <dir>`.
- **Vulkan error / low FPS**: check the **THIS PC - HARDWARE ADVISOR** card in
  `shift-launcher` and apply the recommended preset or update your GPU driver.
- **Android Wi-Fi Sync cannot find PC**: ensure both devices are on the same
  private Wi-Fi network and allow TCP port `47615` and UDP port `47616` in your
  local firewall.
