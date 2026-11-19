# Spec: event restriction values

| | |
| --- | --- |
| Spec ID | `SPEC-EVENT-RESTRICTIONS` |
| Status | complete for every bit the disc uses |
| Written | 2026-11-19, specification role ([CLEAN_ROOM.md](../CLEAN_ROOM.md)) |
| Records | [content findings](../../site/findings/content.toml), group "Event restrictions" |
| Implements it | `shift-engine`, module `data/event_restrictions` |

## What it is

An event can restrict which cars may enter it: one make, one country, a
model-year range, all-wheel drive only, a car-value limit and so on. The
game database (`media/db/gamedb.slt`, SQLite) holds these restrictions in
the `EventRestrictions` table. Each row has the columns `ID`, `EventID`,
`BucketId` and `Value`.

On the base disc there are 120 rows, one per event, and every row has
`BucketId` 0. The game keeps four buckets (0 to 3) per event, so a reader
must accept all four. What buckets 1 to 3 mean is unknown: the disc never
uses them.

The event's class limit (`Events.TargetClass`) is separate from this table
and is applied on top of it. A separate spec will cover it.

## The `Value` string

`Value` is text made of hexadecimal digits. There are at least 10 of them,
always an even count. Read it as a byte string, two digits to a byte.

1. **Mask:** bytes 0 to 4 form a 40-bit mask, least significant byte
   first. A set bit means the restriction is present.
2. **Fields:** after the mask comes one value for each set bit, in
   ascending bit order. Each value is **big-endian**, and its width
   depends on the bit (table below). Bits of width 0 carry no value: the
   bit alone is the restriction.
3. **End:** the string must end exactly after the last field. If bytes are
   left over or missing, the value is malformed (see Errors).

| Bit | Field | Width (bytes) | Meaning | Reference table | Evidence |
| ---: | --- | ---: | --- | --- | --- |
| 8 | year min | 4 | oldest model year allowed (inclusive) | car model year | data, events' own text |
| 9 | year max | 4 | newest model year allowed (inclusive) | car model year | data, events' own text |
| 11 | body family | 1 | the car's body family must equal this ID | `List_FamilyBody.Id` | data, events' own text |
| 15 | region | 1 | the car's region must equal this ID | `List_Region.Id` | data, events' own text |
| 17 | country | 1 | the car's country must equal this ID | `List_Country.CountryID` | data, events' own text |
| 19 | make | 2 | the car's make must equal this ID | `List_CarMake.ID` | data, events' own text |
| 21 | model family | 2 | the car's model family must equal this ID | `List_FamilyModel.Id` | data, events' own text |
| 25 | drive type | 1 | the car's drive type must equal this ID (1 FWD, 2 RWD, 3 AWD) | `List_DriveType.ID` | data, events' own text |
| 34 | flag | 0 | set on 75 of the 120 rows; no effect observed (see below) | — | records |
| 37 | value min | 4 | lowest car value allowed, in credits (inclusive) | the car's price | data, events' own text |
| 38 | value max | 4 | highest car value allowed, in credits (inclusive) | the car's price | data, events' own text |

How the widths were established:

- Each row's byte count after the mask has to equal the sum of the widths
  of its set bits.
- Trying every width in {0, 1, 2, 4} for the 11 bits the disc uses, over
  all 120 rows, gives exactly one solution, the one in the table.

How the meanings were established:

- Each restricted event has a restriction text (`Events.RestrictionDescription`,
  shown on the event ticket). Its English string was compared with the
  decoded field:
  - bit 25 with value 3 on events whose text says all-wheel drive only;
  - bit 19 with the make named in the text;
  - bits 37 and 38 with the credit amounts in the text.
- This held for every restricted event.
- Bits 8 and 9 were also confirmed in the game (see the finding "restriction
  encoding").

Inclusive bounds: an event in the 1980s carries 1980 and 1989. Whether a
missing min or max means "no bound" is implied by the data: rows that carry
only bit 9 read "pre-1980".

### Bits the disc doesn't use

The game names 22 restriction fields, among them car type, model, engine
configuration, engine placement, cylinders, aspiration, power, weight, PI
and car class. On the disc, the value-carrying ID fields sit on odd bits
11, 15, 17, 19, 21 and 25. That fits one layout: each ID field has a pair
of bits from bit 10 up, in this order:

- body family 10-11
- car type 12-13
- region 14-15
- country 16-17
- make 18-19
- model family 20-21
- model 22-23
- drive type 24-25
- engine configuration 26-27
- engine placement 28-29
- cylinders 30-31
- aspiration 32-33

The odd bit of each pair carries the ID. **This layout is probable, not
confirmed.** The even bits, bits 0 to 7, and the other bits above 33 never
occur on the disc. What they mean, and their widths, are unknown.

A reader must reject a value that sets a bit whose width this spec does
not give (see Errors), rather than guess.

### Bit 34

Bit 34 is set on 75 of the 120 rows and has width 0. Those 75 are exactly
the rows whose value isn't all zero, so the flag may mean "this set has
restrictions". Clearing it changed nothing that could be seen: the
Gauntlet still read OPEN and accepted the same cars. Keep it as a decoded
flag with no effect until its use is known.

## The decoded result

The result is one restriction set per bucket. Each set has:

- year min and year max (optional integers);
- value min and value max (optional integers, in credits);
- body family, region, country, make, model family and drive type
  (each an optional ID);
- the bit 34 flag.

A field that isn't set has no limit. A set with no fields admits every car.
A car passes a set when it satisfies every field that is present.

The all-zero value `0000000000`, the 10-digit mask alone, is valid and
admits every car. A database mod can lift an event's restrictions this
way.

## Errors

The decoder rejects:

- a value with fewer than 10 digits, an odd number of digits, or a
  character that isn't hexadecimal;
- a value that sets a bit without a known width;
- a value whose fields run past the end of the string, or that leaves
  bytes unread.

The engine logs the event ID and treats a rejected value as "no
restrictions". That is what the game does when the decode is skipped: its
record set-up starts with every limit at "none". When several errors
apply, which one is reported doesn't matter: the result is the same.

## Questions from the implementation side

Asked on 2026-11-19 by the first implementation. The answers below are
what the records support. Where they say "engine's choice", the game's
behaviour is unknown and doesn't matter for the disc.

| Question | Answer |
| --- | --- |
| How do buckets 0 to 3 of one event combine? | Unknown: the disc uses bucket 0 only. Apply bucket 0 and log any event that has rows in other buckets |
| A `BucketId` outside 0 to 3 | Not on the disc. Engine's choice: ignore the row and log it |
| Two rows for the same event and bucket | Not on the disc. Engine's choice: the last row wins, and it is logged |
| Is the hex case significant? | The disc writes upper case. Accept both, write upper case |
| Are the 4-byte fields signed? | Unknown. Every value on the disc is positive and below 2^31, so unsigned 32-bit is safe |
| A minimum above its maximum | Not on the disc. It admits no car; it is not an error |
| Can a car lack a property? | No. All 176 cars in `Data_Car` on the base disc have a year, base cost, body family, region, country, make, model family and drive type, none empty |

## Tests

- The 120 rows on the base disc decode with no error.
- Each row's decoded fields re-encode to the same `Value` string.
- These synthetic cases decode as stated:

  | Value | Decodes to |
  | --- | --- |
  | `0000000000` | no fields |
  | `0000000004` | flag only (bit 34) |
  | `0003000004000007BC000007C5` | year 1980 to 1989, flag |
  | `0000080004000D` | make 13, flag |
  | `000000020403` | drive type 3, flag |
  | `00000000640000000000003A98` | value 0 to 15,000, flag |

- These synthetic values are rejected:

  | Value | Why |
  | --- | --- |
  | `00000000` | too short |
  | `000000000` | odd length |
  | `0000000001` | bit 32 has no known width |
  | `000008000400` | the make field is truncated |
