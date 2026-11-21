# Installing shift-engine

shift-engine is a native engine that plays a Forza Horizon (2012) game
from your own copy of its files. The download holds only the engine's
programs: no game files, no game code. Setup reads your files once and
writes what the engine needs into a play folder on your machine.

## What you need

- **Your own game files.** The disc's contents, extracted to a folder that
  holds `media/` and `default.xex`. Optionally, your installed title
  update and content packages (the folder that holds `title-update-v4/`
  and `user/.../00000002/`) for the updated database, the add-on cars and
  the Rally pack. shift-engine never downloads game files and cannot play
  without yours.
- **Python 3.10 or newer** for setup (`python --version`).
- **A GPU with Vulkan 1.3.** Tested on Intel UHD 620, AMD Van Gogh (Steam
  Deck) and recent NVIDIA and AMD desktop cards.
- **About 8 GB free** for the play folder (the cars take 7.4 GB).
- **Optional: FFmpeg** on the path, for the engine sounds.

## Windows

1. Unzip `shift-engine-windows-<version>.zip` anywhere.
2. In that folder, run setup with your disc folder (and, if you have
   them, your packages):

   ```powershell
   .\setup.ps1 D:\fh1\disc --state D:\fh1\packages
   ```

3. Run `carview.exe`. With no arguments it finds the play folder
   (`%APPDATA%\shift-engine\play`) and starts the drive.

## Linux and Steam Deck

The Linux build needs glibc 2.38 or newer (current SteamOS, Ubuntu 24.04,
Fedora 39) and the system's ALSA library (`libasound.so.2`, present on
SteamOS and desktop systems); sound goes through PulseAudio or PipeWire
when they run, otherwise ALSA.

1. Unpack `shift-engine-linux-<version>.zip` anywhere with `unzip` or a file
   manager (both keep the programs executable), for example
   `~/shift-engine`.
2. Run setup:

   ```sh
   ./setup.sh ~/fh1/disc --state ~/fh1/packages
   ```

3. Run `./carview`. The play folder is `~/.local/share/shift-engine/play`.
   On the Deck, add `carview` to Steam as a non-Steam game to play in
   Gaming Mode; the pad works as in the table below.

## macOS

Not released yet. The steps will follow Linux's: unpack, `./setup.sh`,
then `./carview`, with the play folder in
`~/Library/Application Support/shift-engine/play`.

## Android

A preview build for arm64 phones and handhelds with Vulkan 1.1 (Android
10 or later). It hasn't been tested on a device yet.

1. Run setup on a computer (Windows or Linux steps above) to make the play
   folder.
2. Install `shift-drive-arm64.apk` on the device, for example with
   `adb install shift-drive-arm64.apk`.
3. Copy the whole play folder to the device as
   `Android/data/net.shiftengine.drive/files/play`, for example with
   `adb push play /sdcard/Android/data/net.shiftengine.drive/files/`.
4. Start Shift Drive. A pad is the easiest way to play.

## Controls

| | Keyboard | Pad |
| --- | --- | --- |
| Throttle | W or Up | Right trigger |
| Brake (held at a stop: reverse) | S or Down | Left trigger |
| Steer | A, D or Left, Right | Left stick |
| Handbrake | Space | A |
| Back on the nearest road | R | |
| Chase or front view | V | |
| Look around | | Right stick |
| Look back (held) | C | Y |
| Pause menu | Esc | Start |
| Map | M | Back |
| Radio station | , and . | D-pad left and right |
| Hide or show the HUD | H | |

The pause menu has the map, the races, the car picker, upgrades, paint,
the 1000 Club, the Autoshow, your career page, settings and reset to
road. Settings take effect the next time you start.

## Your save

Progress is saved in the user folder: `%APPDATA%\shift-engine\drive\save.txt`
on Windows, `~/.local/share/shift-engine/drive/save.txt` on Linux and
`~/Library/Application Support/shift-engine/drive/save.txt` on macOS. To
bring in a career from your own console save, export it with the
project's profile tool and run `carview --import-profile <export folder>`
once.

## Options

`carview --help` lists every option. The common ones:

- `--size 1280x720` window size; `--preset low|high` graphics preset;
- `--no-sound`, `--no-radio`;
- `--no-traffic`;
- `--difficulty easy|med|hard|pro`, `--no-rubberband` for races.

## Settings

To keep options for every run, add `--save-settings` once:

```sh
carview --preset low --no-traffic --difficulty hard --save-settings
```

They are written to `settings.txt` in the user folder (beside `drive/`), one
option per line without the dashes (`preset low`, `no-traffic`). You can
edit the file by hand. Options given on the command line win over the
file, and `--no-settings` ignores it for one run. The file keeps the look
(`preset`, `size`, `msaa`, `shadows`, `bloom`, `no-post`, `exposure`),
sound (`no-sound`, `no-radio`), `traffic`, the race options
(`difficulty`, `no-rubberband`), `camera`, `hour`, `no-collectibles`,
`play` and the mod options. Unknown lines are reported and skipped.

## Mods

See [MODS.md](MODS.md): your own tuning, models and sounds, laid over the
play folder without changing it.

## Troubleshooting

- **"no play folder found"**: run setup first, or pass `--play <dir>`.
- **Black window or a Vulkan error**: update the GPU driver; the engine
  needs Vulkan 1.3.
- **No sound**: install FFmpeg and run setup again, or play with
  `--no-sound`.
