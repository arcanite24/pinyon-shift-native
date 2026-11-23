---
title: Overview
section: Start
order: 1
description: Documentation for Shift Engine, a native C++20 and Vulkan 1.3 clean-room engine for playing your own copy of Forza Horizon (2012) on Windows, Linux, the Steam Deck, macOS and Android.
---

# Shift Engine

A native C++20 and Vulkan 1.3 clean-room engine for playing your own copy of *Forza Horizon* (2012, Xbox 360) on Windows, Linux, the Steam Deck, Apple Silicon Macs and Android.

{{release}}

Shift Engine is not an emulator or a binary recompiler. It reads the archives, models, textures, world geometry, UI scenes, audio banks and database from your own extracted retail disc and runs them natively on a clean-room C++20 engine. Release packages contain **no game files and no game code**: you supply your own copy. See [Legal](legal.md).

## Quick links

- **Get started:** [Install](install.md) on Windows, Linux / Steam Deck, macOS or Android.
- **What works:** [Features](features.md) covers physics, audio, Colorado career and DLC, UI4, controllers, wheels, DualSense, OpenTrack, Photo Mode, replays and achievements.
- **Customize:** [Modding](modding.md) documents `mod.toml`, database patches, live texture reload, custom radio stations, save isolation and LAN sync.
- **Measure:** [Performance](performance.md) explains the Vulkan 1.3 renderer, measured desktop/handheld numbers and `tools/benchmark.py`.
- **Develop:** [Building](building.md) covers compiling from source on all four platforms and running the test suite.

## Requirements

| | |
| --- | --- |
| Game files | Your own lawfully acquired retail disc of *Forza Horizon* (Xbox 360), extracted to a folder holding `media/` and `default.xex`. Optional: your Title Update v4 and DLC packages |
| Systems | Windows 10/11 (x64); Linux or SteamOS 3.7+ (x86-64, glibc 2.38+); macOS 13.3+ (Apple Silicon); Android 10+ (arm64, API 29+) |
| Graphics | Vulkan 1.3 on desktop; Vulkan on Metal via bundled MoltenVK on macOS; Vulkan 1.1+ (system driver or Mesa Turnip) on Android |
| Disk space | About 8 GB free for the baked play folder (`carbake`, optional `soundbake`, and `colorado.index`) |
| Optional | FFmpeg on `PATH` for offline engine sound baking (`soundbake`) and the launcher's Custom Radio Station Builder |

{{features}}

## What is in the repository

The engine libraries (`libs/`), runtime (`apps/carview`), native GUI launcher (`apps/launcher`), offline bakers (`carbake`, `soundbake`, `profile-import`), synthetic unit tests (`tests/`), packaging and benchmark tools (`tools/`), and this site (`site/`). It does **not** hold retail game files, extracted assets, disassembly or generated translations.
