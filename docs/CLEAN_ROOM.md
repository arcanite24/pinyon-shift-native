# Clean-room process for the native engine

The native engine ([REIMPLEMENTATION_PLAN.md](REIMPLEMENTATION_PLAN.md))
ships binaries. Its code therefore must not be derived from the game's code
by transcription. [LEGAL.md](LEGAL.md#native-replacements-of-game-and-library-code)
already sets the rule for native replacements inside Pinyon Shift. This page
applies it to a separate program, splits the work between two roles, and
says how each role is enforced (RF-8, set up by RI-0.7 on 2026-11-19).

## Two roles

| | Specification | Implementation |
| --- | --- | --- |
| Works in | `pinyon-shift` (this repository) | `shift-engine` (provisional name) |
| May use | Ghidra and its MCP server, the generated translation under `.local/generated*`, the running recompilation, traces, frame dumps, the player's files | The specs in `docs/specs/`, the format specs in `docs/formats/`, `site/findings`, the project's own tools and documentation, oracle outputs (traces, images, numbers), public references |
| Must not use | — | Ghidra, the MCP tools, disassembly, generated code, decompiler output |
| Writes | findings, struct layouts, specs: prose, tables, equations, diagrams | engine code and tests |
| Must not write | code, pseudocode, disassembly, decompiler output, game data | guest addresses, generated-code names, game data |

What passes between the roles is a spec. A spec says what a part of the game
does: its inputs, state, update rule, rates, edge cases and tolerances. It
is written in our own words, and it does not say how the original code is
laid out.

## Enforcement

- **Specs are linted.** `python -I tools/re/kb.py lint` also checks
  `docs/specs/*.md` and `docs/formats/*.md`. It refuses:
  - fenced code blocks, except `text` blocks;
  - instruction listings;
  - hex dumps;
  - generated-code idioms such as `ctx.r3`, `REX_LOAD_*` and `__imp__`.
- **The engine is linted.** `shift-engine` has its own check
  (`tools/check-provenance.py`). It refuses guest function names
  (`sub_8xxxxxxx`), guest code addresses and generated-code idioms in
  source files.
- **Agents get the tools of one role.** A specification session runs in
  this repository with the Ghidra MCP server. An implementation session
  runs in `shift-engine`. That repository's `.claude/settings.json` denies
  the Ghidra MCP tools and reads of `horizon1-recomp/.local/generated*`. A
  session never changes role. Work that needs both roles is two sessions,
  joined by a spec.
- **Provenance.** Each engine commit that implements behaviour names the
  spec IDs it follows in its body, for example
  `Spec: SPEC-EVENT-RESTRICTIONS`. Each spec lists the modules that
  implement it.

## Writing a spec

Specs go in `docs/specs/<NAME>.md`, and format specs in
`docs/formats/<NAME>.md`. Start from [the template](specs/TEMPLATE.md). A
spec:

- has an ID, a status, a date and links to the findings it rests on;
- gives each fact its evidence: data, a trace, an experiment or a string.
  Tie a confidence to anything that is not confirmed;
- says what is unknown, and what an implementation must do about it
  (reject, default, or log);
- ends with tests: values the implementation must reproduce, taken from
  synthetic cases or from counts over the player's disc. Never quote game
  data beyond names and counts;
- puts its facts in `site/findings` too, so the address index and the
  reverse-engineering pages stay complete.

## Recorded specs

| Spec | Subsystem | Status | Implemented by |
| --- | --- | --- | --- |
| [SPEC-EVENT-RESTRICTIONS](specs/EVENT_RESTRICTIONS.md) | event rules | complete for the disc's bits | `shift-engine` `libs/data` (`event_restrictions`), commit `34a4804` |
| [WORLD-GEOMETRY](formats/WORLD_GEOMETRY.md) | Colorado render models and prop placements | positions and index lists, `.pgeo` and `.pvsz` placements through the `.pvs` object table; rest of the vertex, materials and the LOD and visibility rule open | `shift-engine` `libs/fh1data` (`rmb`, placements) and `carview` world mode, commits `d55ed64`, `6ed6cdc`..`dc76af7` |
| [FORMAT-CARS](formats/CARS.md) | car models, textures, paint and wheels for the M1 showroom | `.carbin` and `.xds` complete for the disc and official DLC (every model parses, every texture lays out); materials, paint and brakes probable; radial wheel fit, two-tone paint, lamp and node blocks and livery shapes open | `shift-engine` `libs/fh1data` (`carbin`, `xds`), before this spec |
| [FORMAT-COLLISION](formats/COLLISION.md) | Colorado collision tiles, surface types and a spawn point for the M3 drive | tile mesh, `.col` tiling and the per-triangle surface index confirmed against the physics oracle; surface columns named, their use by the tyre model open; prop shapes and some per-triangle fields not decoded | `shift-engine` (not yet) |
| [SPEC-CAR-PHYSICS](specs/CAR_PHYSICS.md) | car parameters, tyre model, assists and input shaping, and the oracle scenarios for the M3 drive | parameters resolve for all 240 cars; longitudinal tyre, suspension units, inertia and the step confirmed on one car; lateral tyre, low-speed steering, drag and the torque chain measured fits; brake torque and the lap gate open | `shift-engine` physics module (not yet) |
| [FORMAT-SHOWROOM-LIGHTING](formats/SHOWROOM_LIGHTING.md) | Autoshow showroom lighting: environment cubes, light terms, drop shadow, post chain and lamp glow | static cube, light direction and colour, tone-map and desaturation constants confirmed; glossy cube, shading equations, composite order and the glow's post pass open | `shift-engine` `carview` (not yet) |
