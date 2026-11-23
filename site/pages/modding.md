---
title: Modding
section: Playing
order: 1
description: How Shift Engine loads mods (mod.toml and mod.txt), database patches, live texture reload, custom radio stations, save isolation and LAN sync.
---

# Modding

A mod in Shift Engine is a folder overlaid on top of your baked play folder and game data while enabled. Your original disc files and baked `.shiftbake` files are never altered: disable or remove a mod and the game returns to stock.

## Mod folders

| Platform | Default `mods/` location |
| --- | --- |
| Windows | `%APPDATA%\shift-engine\mods` |
| Linux and Steam Deck | `~/.local/share/shift-engine/mods` |
| macOS | `~/Library/Application Support/shift-engine/mods` |
| Android | `Android/data/net.shiftengine.drive/files/mods` (synced over LAN) |
| Portable Mode | `<launcher folder>/data/mods` |

You can install `.zip` or folder mods, toggle them on or off, reorder priority and adjust declared mod options on the **Mods** tab in `shift-launcher`, or pass `--mods <dir>` to `carview`. Folders whose names begin with `_` or `.` are ignored.

## `mod.toml` and `mod.txt`

Shift Engine supports `mod.toml` for structured metadata, configurable options, dependencies, conflicts and save-isolation rules:

```toml
name = "asphalt_refresh"
title = "Colorado Asphalt Refresh"
version = "1.0.0"
author = "Mod Author"
description = "Higher-resolution road surface textures."
shares_save = true

[options.texture_tier]
title = "Texture Tier"
default = "4K"
choices = ["2K", "4K"]
```

Legacy `mod.txt` manifests remain supported and are synchronized by `shift-launcher` for load order and enabled state:

```text
title Heavy R34
priority 10
enabled 1
shares_save 1
```

When two enabled mods override the same path, the higher `priority` wins.

## What mods can override

- **Baked cars and audio (`bake/`, `sound/`):** Override car `.shiftbake` definitions or decoded engine audio folders without modifying the base play directory.
- **Database patches (`db/`, `sql/`):** Apply read-only overlay patches to car, event, pricing or tuning tables on top of your disc's `gamedb.slt`.
- **Textures and live reload:** Place replacement textures inside a mod folder to override world or car textures, with live reload support while iterating in game so you do not have to restart the engine after editing a texture.
- **Community packs (UDLC v1.2 and Forza Horizon XE):** `shift-launcher` detects player-supplied UDLC v1.2 and Forza Horizon XE folders and keeps total conversions isolated on their own profile.

## Custom Radio Station Builder

The **Radio** tab in `shift-launcher` builds a self-contained radio mod targeting **Bass Arena (`Radio1`)**, **Horizon Pulse (`Radio2`)** or **Horizon Rocks (`Radio3`)**:

1. Choose the station and select either **Replace stock tracks** or **Extend stock playlist**.
2. Add a folder of `MP3`, `WAV`, `FLAC`, `OGG`, `M4A` or `OPUS` audio files.
3. Optionally set a custom station name and a `210×100` PNG station logo.
4. Build the mod. Using FFmpeg, the launcher encodes tracks to 48 kHz 160 kbps CBR MPEG-1 Layer III, writes an FSB4 bank with `SongStart`, `EventStart` and `IdentStart` sync points, updates `RadioSoundbankInfo_Music.xml` and `RadioSystem.xml`, and outputs a ready-to-enable mod in `mods/` with `shares_save = true`.

## Save isolation and sync

Shift Engine separates saves by mod impact so experimental or gameplay-altering mods do not corrupt your career progress:

- **Visual, texture and radio mods (`shares_save = true`):** Share your standard career profile (`user`) and can be toggled on or off freely.
- **Database and gameplay mods (`db/` or `sql/` patches, `shares_save = false`):** Automatically route to an isolated `user-modded` save copy.
- **Total conversions (`profile = "user-xe"`):** Keep a dedicated profile directory.

### Backups, Xenia import and LAN sync

- **Automatic backups:** Saves are backed up automatically before overwrites and can be inspected or restored from the launcher or in-game save screen.
- **Xenia save import:** `profile-import` (and the launcher's import action) reads your existing Xbox 360 / Xenia `ForzaProfile` save tree and converts progress, garage cars, signs and unlocks into Shift Engine's native profile format.
- **LAN sync:** `shift-launcher` can sync baked assets, enabled mods, custom radio stations and saves to Android devices over your local Wi-Fi network without cloud services or USB cables.

## Sharing mods cleanly

Distribute only files you authored yourself (manifests, SQL/config patches, original textures or audio). Never redistribute baked `.shiftbake` files, decoded stock sound banks or extracted retail game files. See [Legal](legal.md).
