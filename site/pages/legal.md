---
title: Legal
section: Project
order: 1
description: Clean-room policy, bring-your-own-disc requirements, automated provenance and binary-content verification, and licences for Shift Engine.
---

# Legal

Shift Engine distributes only independently written C++20 source code, shaders, build scripts, tools and documentation. Releases do **not** include *Forza Horizon*, extracted assets, executable modules, title updates, DLC packages, generated translations or any game data.

## Bring your own disc

To play, you supply your own lawfully acquired copy of *Forza Horizon* (2012, Xbox 360), extracted to a local folder (`media/` and `default.xex`), plus any Title Update v4 or DLC packages you own.

- Setup reads and bakes your files locally on your machine.
- Nothing downloads, uploads, shares or helps locate copyrighted game files.
- Baked `.shiftbake` files, decoded stock audio banks and `colorado.index` are derived from your disc and must remain on your own devices.

## Clean-room development policy

This repository is written from format and behavioral specifications rather than from disassembly, decompiled output or recompiled guest code.

- Source files in this repository never contain disassembly listings, decompiler output, guest function names, guest code/data addresses or generated translation macros.
- Unit tests run on synthetic bytes created in code; corpus checks that validate behavior against a disc read only from local environment variables (`SHIFT_GAME_ROOT`, `SHIFT_STATE_ROOT`) and never write game data into the repository.

## Verification checks

Two automated checks enforce these boundaries:

- **Source provenance (`tools/check-provenance.py`):** Runs in CI on every push and pull request. Scans tracked source files and fails if any guest function symbol, guest address range or generated-code register/macro pattern appears.
- **Release binary-content check (`tools/check-binary-content.py`):** Inspects every file in a built release folder or `.zip` archive (run via `tools/package.py --check --game-root <disc>`) for provenance text, retail game filenames or container headers (XEX, XPR, SQLite), PowerPC instruction sequences (`blr` / `mflr r0`), and 32-byte content-hash fingerprints against your game tree. See [docs/RELEASE_CHECK.md](https://github.com/arcanite24/shift-engine/blob/main/docs/RELEASE_CHECK.md).

## Do not redistribute

- Disc images, `default.xex`, title updates (`default.xexp`), DLC containers or extracted `media/` files.
- Baked `.shiftbake` files, decoded stock engine sound banks or screenshots/captures derived from retail game assets in pull requests.
- Modified release archives bundling retail game content.

## Licence and third-party notices

Independently written code in this repository is licensed under the [GNU General Public License v3.0 or later](https://github.com/arcanite24/shift-engine/blob/main/LICENSE). That licence covers only Shift Engine's own code and grants no rights to *Forza Horizon* or any files you supply.

Third-party libraries fetched at build time (`libmspack` LGPL-2.1, `SDL3` zlib, `Vulkan-Headers` Apache-2.0/MIT, `volk` MIT, `VulkanMemoryAllocator` MIT, `SQLite` public domain, `stb` MIT/public domain, `dr_libs` MIT-0/public domain) are pinned in `cmake/Dependencies.cmake` and documented in [THIRD_PARTY_NOTICES.md](https://github.com/arcanite24/shift-engine/blob/main/THIRD_PARTY_NOTICES.md).

Microsoft, Xbox, Turn 10 Studios, Playground Games and *Forza Horizon* are the property of their respective owners. Shift Engine is not affiliated with or endorsed by them. This page describes the project's policy and is not legal advice.
