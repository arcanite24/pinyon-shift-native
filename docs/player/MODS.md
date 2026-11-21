# Mods

A mod is a folder of files that replace the play folder's own files
while it is on. The play folder itself is never changed: take a mod out
(or turn it off) and the game is as setup made it.

## Where mods go

`mods/` in the user folder: `%APPDATA%\shift-engine\mods` on Windows,
`~/.local/share/shift-engine/mods` on Linux. Each mod is one folder:

```text
mods/
  heavy-r34/
    mod.txt
    bake/cars/nis_skylinegtrvspecii_02.physics.txt
  quiet-engines/
    mod.txt
    sound/NIS_SkylineGTRVSpecII_02/...
```

`carview --mods <dir>` uses another folder; `--no-mods` turns them all off
for a run. The window lists the mods it found at start-up (`mod: name
(title), N files, on`).

## mod.txt

```text
title Heavy R34
priority 1
enabled 1
```

All lines are optional, but the file must exist. `enabled 0` turns a mod
off. When two mods hold the same file, the higher `priority` wins, then
the folder name in alphabetical order. A folder whose name starts with
`_` or `.` is skipped.

## What a mod can replace

The paths are the play folder's, below `bake/` and `sound/`:

| File | What it is |
| --- | --- |
| `bake/cars/<car>.physics.txt` | the car's tuning: mass, engine, gearing, tyres (the values setup read from your game's database) |
| `bake/cars/<car>.shiftbake` | the car's model and textures |
| `bake/wheels/<wheel>.shiftbake` | a wheel or tyre model |
| `bake/shared.shiftbake` | the shared car textures |
| `sound/<car>/` | the car's whole engine sound folder |

Start from a copy of the play folder's file and change it: for example,
`List_UpgradeCarBodyWeight.Mass` in a `.physics.txt` is the car's mass in
kilograms. Model and sound files are what `carbake` and `soundbake` write.

A mod replaces files only. It cannot add a car, a race or a place in the
world: those also need rows in the game's database, which the engine reads
from your own game files.

## Sharing mods

Share only files you made. A `.shiftbake` or a sound folder made by
`carbake` or `soundbake` from your copy of the game holds that game's
models, textures or sounds, so it is not yours to share. Tuning files
hold the game's numbers too; share the lines you changed, not a whole
file.
