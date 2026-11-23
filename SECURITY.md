# Security Policy

## Supported versions

Security fixes are applied to the `main` branch and included in the next release.

| Version | Supported |
| --- | --- |
| `main` / latest release | Yes |
| Older releases | No |

## Scope

Security reports are most useful for issues in:

- **Format and archive parsers** (`libs/fh1data`, `libs/cars`, `libs/audio`, `libs/profile`) — memory safety bugs, out-of-bounds reads/writes, integer overflows, or path traversal when reading untrusted archives, mods, `.shiftbake` files, STFS packages, or saves.
- **Launcher Wi-Fi Sync server** (`apps/launcher/wifi_sync.cpp` and `apps/carview/android/.../WifiSyncClient.java`) — authentication/pairing bypass, path traversal, or request handling bugs on the local LAN sync ports (TCP `47615`, UDP `47616`).
- **Release and update integrity** (`apps/launcher`, `tools/package.py`, release checksums).

## Reporting a vulnerability

Please **do not** open a public GitHub issue for security vulnerabilities.

Instead, report it privately through [GitHub Security Advisories](https://github.com/arcanite24/pinyon-shift-native/security/advisories/new) on this repository.

Include:

- A clear description of the issue and the affected file or subsystem.
- Minimal reproduction steps or a synthetic test input that triggers the bug.
- Expected vs. actual behavior and potential impact.

**Do not attach retail game files or copyrighted assets** to a security report. Use minimal synthetic inputs wherever possible.

## Release verification

Every official GitHub Release publishes a `SHA256SUMS.txt` file alongside its archives. Verify downloaded archives against `SHA256SUMS.txt` before running them.
