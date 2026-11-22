# Mods & Custom Radio Stations

A mod is a folder laid over your play folder and game data while enabled. Your
original disc files and baked play folder are never modified: disable or remove
a mod and the game returns to stock.

## Where mods go

- **Windows:** `%APPDATA%\shift-engine\mods`
- **Linux / Steam Deck:** `~/.local/share/shift-engine/mods`
- **macOS:** `~/Library/Application Support/shift-engine/mods`
- **Android:** `Android/data/net.shiftengine.drive/files/mods` (synced automatically via Wi-Fi Sync)
- **Portable Mode:** `<launcher folder>/data/mods`

Use the **MODS** tab in **`shift-launcher`** to install `.zip` or folder mods
(or drag and drop them onto the window), toggle mods on/off, reorder load
priority, edit declared `mod.toml` options, and review save-isolation notices.

## `mod.toml` and `mod.txt`

`shift-engine` and `shift-launcher` support both `mod.toml` (rich metadata,
options, dependencies, conflicts, save isolation) and legacy `mod.txt`:

```toml
name = "my_cool_mod"
title = "My Cool Mod"
version = "1.0.0"
author = "Modder"
description = "Upgraded asphalt textures and balanced tuning."
shares_save = true

[options.texture_tier]
title = "Texture Tier"
default = "4K"
choices = ["2K", "4K"]
```

Legacy `mod.txt` (also maintained automatically by `shift-launcher` so every
build reads load order and enabled state):

```text
title Heavy R34
priority 10
enabled 1
shares_save 1
```

Higher `priority` wins when two mods touch the same file. Folders starting with
`_` or `.` are ignored.

## Save Isolation

- **Visual / audio / radio mods (`shares_save = true`):** Safe to toggle on and
  off anytime while continuing your normal career save (`user`).
- **Database / roster / gameplay mods (`db/` or `sql/` patches, `shares_save = false`):**
  Play on an isolated copy (`user-modded`) so your main career save is never
  altered.
- **Total conversions (`profile = "user-xe"`, e.g. Forza Horizon XE):** Run on
  their own dedicated save profile.

## Custom Radio Station Builder

Open the **RADIO** tab in `shift-launcher` to replace or extend **Bass Arena
(`Radio1`)**, **Horizon Pulse (`Radio2`)**, or **Horizon Rocks (`Radio3`)**:

1. Pick the target station and choose **REPLACE STOCK TRACKS** or **EXTEND STOCK PLAYLIST**.
2. Drop a folder of `MP3`, `WAV`, `FLAC`, `OGG`, `M4A`, or `OPUS` files onto the
   launcher.
3. Optionally enter a **Custom Station Name** and select a **210×100 PNG**
   station logo.
4. Click **ENCODE & PACK CUSTOM RADIO MOD**. `shift-launcher` uses FFmpeg to
   encode tracks to 48 kHz 160 kbps CBR MPEG-1 Layer III, rebuilds the FSB4
   bank with `SongStart` / `EventStart` / `IdentStart` sync points, updates
   `RadioSoundbankInfo_Music.xml` and `RadioSystem.xml`, and writes a ready-to-use
   mod under `mods/<name>/` (with `shares_save = true`).

## DLC, Title Update v4, UDLC v1.2 & Forza Horizon XE

Open the **DLC & V4** tab in `shift-launcher` to verify and import your own
Xbox 360 STFS packages (`CON`/`LIVE`/`PIRS`) or extracted offer folders, mount
Title Update v4 (Horizon Rally + 1000 Club), and detect community **UDLC v1.2**
and **Forza Horizon XE** packages.

## Sharing Mods

Share only assets and patches you authored yourself. Never distribute baked
`.shiftbake` files, decoded stock sound banks, or retail game assets extracted
from the disc.
