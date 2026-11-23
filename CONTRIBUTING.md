# Contributing to Shift Engine

Thank you for contributing. Please read the clean-room rule first: it is a hard boundary for every pull request.

## Clean-room rule

Shift Engine is an independently written, clean-room implementation. Never commit, quote, or link to:

- Retail game files, disc images, extracted assets, textures, audio, databases, or `.shiftbake` files.
- Disassembly listings, decompiler output, Ghidra exports, guest function names (`sub_82...`), or guest addresses (`0x82...` / `0x83...`).
- Generated translation code or register-access macros from a recompilation.
- Screenshots, renders, or logs produced from retail game data (`.gitignore` excludes `*.png` and `*.log`).

Unit tests under `tests/` that run in CI (`ctest -LE corpus`) must operate strictly on synthetic bytes generated in code. Tests that inspect a local disc must be labelled `corpus`, read paths only from `SHIFT_GAME_ROOT` / `SHIFT_STATE_ROOT`, and exit with CTest skip code `77` when those environment variables are unset.

Run the provenance check before every commit:

```sh
python tools/check-provenance.py
```

## Development workflow

### Build and test

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build -LE corpus --output-on-failure
```

On Windows PowerShell:

```powershell
.\tools\build-windows.ps1
ctest --test-dir build -LE corpus --output-on-failure
```

Run tool and site checks when touching tooling, packaging, or documentation:

```sh
python tools/check-provenance.py
python tools/benchmark.py --self-test
python tools/android-apk.py --validate-only
python site/build.py
```

### Shaders

If you modify any GLSL shader under `apps/carview/shaders/`, recompile and commit the matching prebuilt SPIR-V binaries in `apps/carview/shaders/prebuilt/` so contributors and CI runners without a GLSL compiler still build cleanly:

```sh
python tools/regenerate-shaders.py [--compiler /path/to/glslc]
```

### Documentation site

When a change affects supported platforms, player-facing features, settings, build steps, modding, or measured performance numbers, update the corresponding pages in `site/pages/` and `site/features.toml` in the same pull request, then verify:

```sh
python site/build.py
```

`site/build.py` validates every internal page link and `#anchor` in `site/_build/` and fails on broken references.

## Commit messages

Use [Conventional Commits](https://www.conventionalcommits.org/):

```text
<type>(optional-scope): <imperative summary>
```

- Types: `feat`, `fix`, `perf`, `refactor`, `test`, `docs`, `build`, `ci`, `chore`.
- Lowercase, imperative summary, no trailing period, at most 72 characters.
- Keep each commit focused on one logical change.
- Explain *why* in the commit body when not obvious, wrapped at 72 characters, and include a `Tests:` line listing the validation run.
- Do not include hardcoded local user paths, private machine names, or personal credentials anywhere in code, tests, or commit messages.

## Licence

By submitting a contribution, you agree that your work is independently authored and licensed under the project's [GNU General Public License v3.0 or later](LICENSE).
