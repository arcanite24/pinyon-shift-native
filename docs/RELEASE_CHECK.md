# The release check (M7)

A release package must hold no guest code and no game data. The rule
below is the definition (the plan names only those two words);
`tools/check-binary-content.py` implements it and `tools/package.py
--check` runs it on every package.

## What is checked

Every file of the package (a folder or its `.zip`), whatever its type:

| Check | Fails when | Why |
| --- | --- | --- |
| Text | the file's bytes contain a guest function name (`sub_8…`), a guest code or data address (`0x82…`, `0x83…`), a generated-code register access (`ctx.` and a register name) or a generated-code macro: the list of `tools/check-provenance.py` | the clean-room rule for sources, applied to what ships |
| Name | the file is named like a file of the game (disc, title update, content packages), or has a game-data extension (`.xex`, `.xexp`, `.xpr`, `.xds`, `.bix`, `.slt`, `.pgeo`, `.col`, `.owt`, …) | no game file copied in under its own name |
| Format | the file starts as an XEX executable, an XPR resource or a database | no game container under another name |
| Guest code | the PowerPC return word (`blr`, 4E800020) or link-save word (`mflr r0`, 7C0802A6) occurs more than 64 times | the game is PowerPC code; native x86-64 and ARM64 binaries hold these words only by chance |
| Game data | the file shares 8 or more fingerprints with the game's files | a copy of game bytes, at any offset and inside any other file |

## Fingerprints

A fingerprint is a hash of a 32-byte window. Every window of a file is
hashed and kept when its mixed hash falls in 1/512 of the range, so the
kept set depends on the content, not on where it sits: a copy embedded
at any offset keeps the same fingerprints. The reference set covers
every file of the disc (`media/`, the executables), the title update
(`title-update-v4/`: `default.xexp` and `media.zip`) and every installed
content package (`user/.../00000002/`). The state root's own `dlc/*.json`
records are not game data and are left out.

Windows that also occur in the system's own libraries (the 120 largest
DLLs under `System32`: padding, zero runs, common tables) are generic
and dropped from the reference first. The set is cached in
`out/content-fp.npy`; delete it after the game files change.

Self-test: a 64 KiB slice of the game's executable embedded at an odd
offset in random bytes shares 143 fingerprints with the executable;
200 KB of random bytes share none. A copy of 4 KiB or more is expected to
fail; smaller ones may pass.

## Running it

```sh
python tools/package.py --build build-play --platform windows --check \
    --game-root <disc root> --state-root <state copy>
python tools/check-binary-content.py <package dir or zip> \
    --game-root <disc root> --state-root <state copy>
```

Exit 0 and `binary-content check: … pass` is the M7 pass. The check
reads the game's files only to hash them and never copies them.
