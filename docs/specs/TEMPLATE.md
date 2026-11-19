# Spec: <subsystem or rule>

| | |
| --- | --- |
| Spec ID | `SPEC-<NAME>` |
| Status | draft / complete / complete for … |
| Written | <date>, specification role ([CLEAN_ROOM.md](../CLEAN_ROOM.md)) |
| Records | links to the `site/findings` groups this rests on |
| Implements it | the engine module, once one exists |

## What it is

What this part of the game does, for the player and for the rest of the
game. Its inputs and outputs, in terms of data the engine has: database
tables, files, input and other subsystems' state.

## State

What it keeps between updates. Give units and ranges, and say which other
parts read it.

## Rule

How the state and outputs follow from the inputs. Use prose, tables,
equations and diagrams (`text` blocks). No code and no pseudocode
(CLEAN_ROOM.md).

## Rates and timing

When it runs, how often, and what it depends on: frame rate, the
simulation step, real time.

## Unknowns

What isn't known, the confidence of what is probable, and what an
implementation must do meanwhile.

## Tolerances

How closely an implementation must match the oracle, and how that is
checked: a trace field and its tolerance, an image metric, an A/A spread.

## Tests

Cases the implementation must reproduce, either synthetic or counts over
the player's disc.
