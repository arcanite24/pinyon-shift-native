# Spec: the radio for the M4 drive

| | |
| --- | --- |
| Spec ID | `SPEC-RADIO` |
| Status | draft: files, name resolution, decoding, counts and the cycle order confirmed; the background rule, shuffle, ident and DJ sequencing, levels and the popup measured on two oracle runs (probable); the start station, the picker's exact rule, ducking, the new-track popup and the 3D location stations open |
| Written | 2026-11-20, specification role ([CLEAN_ROOM.md](../CLEAN_ROOM.md)) |
| Records | [audio findings](../../site/findings/audio.toml), groups "Radio banks and playlists", "Radio presentation" and "Radio playback"; group "Mixer and output" ("mixer snapshots multiply") |
| Implements it | `shift-engine`, audio module (not yet) |

## What it is

Plan item M4 ([REIMPLEMENTATION_PLAN.md](../REIMPLEMENTATION_PLAN.md)) needs
the game's radio while the player drives in free roam: three music
stations with their idents and DJs, switched on the D-pad, with the
station popup on the HUD. This spec says where the stations, tracks and
clips are on the player's disc, how to decode them, how the game plays and
switches them, how loud the radio is against the engine, and what the
popup shows. A sample-exact copy of the game's choices is not the goal: the
picks are random in the game too. Where a rule was not traced, the spec
gives an interim rule.

Inputs, all from the player's files and the engine's own state:

- `media/audio/radio/RadioSystem.xml`: stations, playlists, idents, DJ line
  groups and the system levels;
- `media/audio/radio/Radio_Music.fsb` and `Radio_VO_<lang>.fsb`: the audio;
- `media/audio/radio/RadioSoundbankInfo_Music.xml` and
  `RadioSoundbankInfo_VO_<lang>.xml`: the sample index and sync points of
  every clip;
- `media/audio/AudioMixerSnapshots.xml`: the category levels (shared with
  [SPEC-ENGINE-SOUND](ENGINE_SOUND.md));
- the popup logos `Horizon.zip/Hud/Radio/RS_*.xds` (findings "station
  popup logos");
- the D-pad, the game state (free roam, paused, loading) and the profile's
  RADIO STATION, RADIO DJ and RADIO VOLUME options.

v4 and the 21 DLC packages add no radio files: v4's `media.zip` has no
radio member, and none of the 57 package archives holds a radio member
(only two cars' gauge textures have "radio" in their names). v4 adds a
rally radio mode (`SetRadioRallyMode`, findings "classes the title update
adds"), out of scope here.

Evidence comes from:

- the files, read with `tools/re/radio.py census` and decoded with
  `tools/re/radio.py decode` (FFmpeg);
- two hidden oracle runs from the `acc9-stock` seed, parked at idle with
  every mix category but the engine and music at 0:
  `config/render-tests/fh1-radio-cycle.fh1test` (run A: five D-pad right
  presses, then two left, 45 s apart, a 40 s capture of the native 6-channel
  mix after each, HUD captures 10 to 360 frames after each press) and
  `fh1-radio-switchback.fh1test` (run B: switching away and back after 10,
  20 and 40 s inside three 60 s captures);
- `tools/re/radio.py match`, which finds the track and position each 1 to
  4 s window of a capture plays by cross-correlation against every decoded
  track, ident and DJ line of the three stations (scores 0.9 to 0.99 on
  music, under 0.3 for a wrong clip);
- the flow files in `gamemodes.zip` for the behaviours that drive the
  radio.

This spec quotes names, counts and measured values only. The playlists,
titles, artists, sync points, levels and fade times are read from the
player's files at run time; the spec names the attributes that hold them.

## Tracks (item 1)

### Files and format

| File | Holds | Samples | Format |
| --- | --- | --- | --- |
| `Radio_Music.fsb` | every music track, including the 3D location stations' edits | 127 | FSB4, MPEG-1 Layer III, 48 kHz, 160 kbps CBR; 69 stereo (Radio1-3 and the off station), 58 mono (the location stations) |
| `Radio_VO_<lang>.fsb` | idents, DJ lines, festival updates and interviews | 536 (EN) | FSB4, MPEG |
| `RadioSoundbankInfo_Music.xml` | per track: sample index, sync points in ms, length | 127 entries | XML |
| `RadioSoundbankInfo_VO_<lang>.xml` | per clip: sample index, `OffsetStartNextTrack`, length | 536 entries | XML |

The bank layout is the findings record "FSB4 sample header layout": a
48-byte header, 80-byte sample headers, and on music samples a `SYNC` block
with three points, `SongStart`, `EventStart` and `IdentStart` (one sample
spells the first `SongSTart`). Sample data follows the headers, each sample
aligned to 32 bytes.

**Resolve names through the info file, not the bank.** A sample header
keeps only 29 characters of a name, and many DJ lines share their first 29
characters. A clip named in `RadioSystem.xml` is the sample at its info
file entry's `SoundBankIndex`. With that rule every name resolves (counts
below). Matching by the truncated bank name fails for 133 to 147 names
per player station.

**Languages.** The disc has VO banks for EN, BR, FR and MX, but info files
for 15 languages. Interim rule: use the player's language when its bank
exists, else EN.

**Decoding.** Each MPEG sample is a run of Layer III frames. Short samples
can carry padding between frames, which must be dropped before FFmpeg's mp3
decoder locks on (as `tools/re/media-decode-check.py` does). The first
track and first ident of every station, 18 samples, decoded to exactly
their declared lengths. **Bake each clip to PCM at install time**; at about
680 MB of MP3 for the music, keep the baked music compressed (for example
Opus or Vorbis) or stream it from the MP3s.

### RadioSystem.xml

The file is not strict XML. A tolerant reader must escape bare `&` (in
artist names) and accept an attribute named `3dPanLevel`. Track entries
spell the sample attribute `fmodname`, idents and dialogue `fmodName`.

It has one `System` element, nine `RadioStation` elements and one
`FestivalUpdates` list. A station holds:

| Element | Children | Meaning |
| --- | --- | --- |
| `Playlist` (`noRepeat`) | `MusicTrack`: `name`, `artist`, `fmodname`, `likelihoodDaytime`, `likelihoodMorning`, `likelihoodEvening`, `likelihoodNight` | the tracks, with the title and artist the popup shows |
| `Idents` (`noRepeat`) | `Ident`: `fmodName` | station jingles between tracks |
| `DJBookendDialogueIn`, `DJBookendDialogueOut` (`noRepeat`) | `SimpleDialogue` | lines around a DJ break; all `DJ_Blank` on the disc |
| `DJRegularDialogue` (`noRepeat`) | `SimpleDialogue` | the DJ's general lines (facts, reminders) |
| `DJSpecialDialogue` | `SpecialDialogue` | barn-find and location hints, triggered by gameplay |
| `DJImmediateDialogue` | `SpecialDialogue` | story lines, triggered by the flows |
| `DJFestivalUpdateLeadIn`, `DJFestivalUpdateLeadOut` (`noRepeat`) | `SimpleDialogue` | lines around a festival update or interview |

### Counts per station

From `tools/re/radio.py census` (disc, EN):

| Station | Popup logo | DJ | Tracks | Music | Idents | Regular / special / immediate / festival in + out DJ lines |
| --- | --- | --- | --- | --- | --- | --- |
| `Radio1` Horizon Bass Arena | `RS_BassArena` | Scott Tyler | 22 | 122 min 38 s | 11 | 72 / 29 / 21 / 10 + 10 |
| `Radio2` Horizon Pulse | `RS_Pulse` | Holly Cruz | 23 | 98 min 52 s | 10 | 65 / 29 / 21 / 10 + 10 |
| `Radio3` Horizon Rocks | `RS_2Eleven` | Phoenix Fox | 22 | 82 min 4 s | 14 | 79 / 29 / 21 / 10 + 10 |
| `Radio4_Silent` (off) | `RS_Off` | none | 2 blank | 10 min 16 s | 6 blank | blank |
| `Radio5_3d_PreRace` | none | none | 21 | 85 min 13 s | 4 blank | none |
| `Radio6_3d_Autoshow` | none | none | 8 | 43 min 52 s | 4 | none |
| `Radio7_3d_Workshop` | none | none | 11 | 45 min 59 s | 3 | none |
| `Radio8_3d_Paintshop` | none | none | 11 | 48 min 17 s | 3 | none |
| `Radio9_3d_Streetrace` | none | none | 5 | 21 min 18 s | 3 | none |

- The three player stations hold 67 distinct tracks, each in one station.
  Three idents (`R123_IdentShared_01` to `_03`) are in all three.
- Every track's four time-of-day likelihoods are 1 on the disc.
- `noRepeat`: playlists 15 on Radio1-3, idents 6 (Rocks 8), regular DJ
  lines 40, bookends 2, festival lines 8.
- All 125 playlist entries and every ident and DJ name resolve; 29
  festival updates resolve. Two music samples and 47 EN VO samples are
  named by no station.
- Track lengths: Bass Arena 228 to 476 s, Pulse 174 to 381 s, Rocks 159 to
  369 s.

**M4 needs Radio1-3 and off.** The location stations (`Radio5`-`Radio9`)
are mono edits played from points in the world (the Autoshow, the
workshop, pre-race and street-race hubs). Their `volume`, `reverbWetLevel`
and `3dPanLevel` attributes are for that and are out of scope.

### Sync points

| Point | Where | Meaning (from the runs, probable) |
| --- | --- | --- |
| `SongStart` | track | always 0 |
| `EventStart` | track, 13 to 134 s (median 29 s) | end of the intro: the point a DJ line over the intro must finish by (interim) |
| `IdentStart` | track, 0.2 to 16.7 s before the end (median 2.0 s) | the ident starts here, over the track's tail |
| `OffsetStartNextTrack` | ident, 3.8 to 13.2 s into idents of 5.2 to 13.9 s | the next track starts here, under the ident's tail |

## Playback (item 2)

### Stations and the cycle

The player stations are, in cycle order:

```text
D-pad right:  Bass Arena -> Pulse -> Rocks -> off -> Bass Arena
D-pad left:   Bass Arena -> off -> Rocks -> Pulse -> Bass Arena
```

Off is a stop in the cycle (`Radio4_Silent`, `isOffStation`), not a
separate control. This is confirmed from the popups: run A pressed right
five times from off and saw Bass Arena, Pulse, Rocks, RADIO OFF, Bass
Arena, then left twice and saw RADIO OFF, Rocks; run B saw right from
Rocks give RADIO OFF and left from off give Rocks.

The flows also set a station by name (`bass_arena`, `pulse`, `2_eleven`,
`radio_off`), can lock station switching, and start the opening drive on a
fixed two-track playlist. M4's free roam needs none of these.

**Start station.** Two runs from the same seed started free roam once on
off (run A) and once on Rocks (run B). Run A ended on Rocks, but each run
plays a fresh copy of the seed and the seed's profile was not written, so
no carried state was found; it may be chance. The rule is open. Interim
rule: start on the profile's RADIO STATION option, and on Bass Arena when
there is none.

### Live radio: what a station does while not selected

Each station keeps its own current track and position. **While any station
is selected, every station advances in real time; while the radio is off,
none does.** Switching therefore joins the other station where it has got
to, not at a track start, and turning the radio off and on again resumes
mid-track.

| Case (oracle) | Away | Radio on while away | Position change |
| --- | --- | --- | --- |
| Rocks, off and back (run B) | 10 s | 0 s | +0.0 s (resumed) |
| Rocks, off, Bass Arena, off and back (run B) | 61 s | 20 s | +20.2 s |
| Rocks, off, Bass Arena, off and back (run A) | 135 s | 45 s | +45 s into the next track, if that track was `R3_AwayFromHere` from its start (the capture ended 2 s into the next track, too early to tell it from `R3_Animal`) |
| Bass Arena, Pulse, Rocks, off and back (run A) | 135 s | 90 s | +95 s (5 s more than the rule; the next track started under a DJ line, see Unknowns) |

When the radio first came on in run A the three stations stood at 88, 73
and 39 % of the way through different tracks. **A station starts on a
random track at a random point.**

An engine can keep one clock per station, or one clock for the radio and
each station's offset into its schedule. Only the selected station needs
decoding.

### Order of tracks, idents and DJ lines

The order within a station:

```text
track A ... IdentStart(A)                      end of A
                |--- ident ------------------------------|
                                 OffsetStartNextTrack
                                        |--- track B ...
                                        |--- DJ line --|   (over B's intro)
```

- A track plays from its start (or a joining point) to its `IdentStart`
  point, where an ident starts. The rest of the track keeps playing under
  the ident.
- The next track starts at the ident's `OffsetStartNextTrack`, under the
  ident's last seconds.
- In both changes captured to the next track, a DJ regular line
  started within 0.2 s of the next track and talked over its intro (Bass
  Arena: an Autoshow reminder; Rocks: a fact about a rival).
- No crossfade: the clips overlap at full level. The overlaps are part of
  the recordings' timing.

Observed: Bass Arena `R1_ThePower`, `R123_IdentShared_01`, DJ line, then
`R1_Illmerica`; Rocks `R3_BackOfYourNeck`, `R3_Ident_06`, DJ line, then
probably `R3_AwayFromHere`; Pulse `R2_SpiritOfTheNight`, then
`R2_Ident_05`.

**Shuffle.** The next track is not the playlist's next entry: on Bass
Arena entry 16 was followed by entry 4 (no other track fits the time to
the return), and on Rocks entry 1 by entry 20 or entry 0. Interim rule
for the picker, from the attribute names (probable): pick uniformly at
random, weighted by the time-of-day likelihood (all 1 on the disc), among
the entries not among the last `noRepeat` played. The same for idents and
DJ lines with their own `noRepeat`.

**DJ lines.** `System maxTracksBetweenDJ` allows a DJ break after every
track on the disc. Interim rule: after each ident,
play one regular line from the station's `DJRegularDialogue` over the next
track's intro, when the line ends before that track's `EventStart` and the
RADIO DJ option is on; else play no line. Bookends are `DJ_Blank` (a silent
clip) and can be skipped. Special, immediate and festival lines
come from gameplay events and the flows; leave them out of M4.

### Off

Off plays nothing: its playlist is two blank tracks with blank idents and
lines. Treat it as silence that stops the other stations' clocks.

### Switching fades

`System stationChangeFadeTimeUser` (a D-pad change) and
`stationChangeFadeTimeNormal` (a change by the game) give the fade times.
Interim rule: on a D-pad change, fade the old station out and the new one
in, each over `stationChangeFadeTimeUser`. The captures show the new station within
about 1 s of the press; the fade itself was not resolved.

## Mixing (item 3)

### Levels

The radio's level is a product of:

```text
radio gain = musicLevel x masterLevel x RADIO VOLUME option
             x GameMusic level of each active snapshot group
```

| Factor | Where |
| --- | --- |
| Music level and master level | `System musicLevel`, `masterLevel`; idents and DJ lines use `identLevel` and `dialogueLevel` instead of `musicLevel` (probable) |
| Free-roam level | `GameMusic` in the `FreeRoam` snapshot (Gameplay group); the player's engine is `FocusCar` in the same snapshot |
| Place | `GameMusic` in the Location group (`Festival` is lower than the others) |
| Speed | `GameMusic` in `TopSpeed` and `LowSpeed` (Speed group; slightly louder at top speed) |
| Speech | `SatNavPlaying` (SatNav group) and `VOPlaying` (VO group) lower the music |
| Game states | `Paused`, `Loading`, `UIExterior`, `RaceEncounter` and `Barnfind` (Gameplay group) lower the music |

Snapshot groups multiply (findings "mixer snapshots multiply"), and each
snapshot fades to its level over its `fadetime`. The `AudioEngineConfig.xml`
`gameMusic` level measured no effect in free roam (findings) and
should be ignored.

**Measured.** Parked at idle in free roam, the radio played stereo on the
front left and right channels only (centre, LFE and surrounds carried the
engine and the world), at 0.08 to 0.10 RMS per front channel against 0.008
for the idling engine alone. **The radio sits about 20 dB above an idling
engine.** The level under throttle was not captured.

### Ducking

| When (snapshot) | What drops |
| --- | --- |
| A DJ line plays (`RadioDJSpeaking`) | the player's engine (`FocusCar`) and sat-nav; not the music |
| A festival update (`RadioFestivalUpdate`) | the engine, and the music a little |
| Sat-nav speaks (`SatNavPlaying`) | the music, and the engine less |
| Story speech (`VOPlaying`) | the music and the engine |
| Pause, loading | the music |

Each snapshot fades to its `destvolume` over its `fadetime` and back over
its `returnfadetime`. `System duckValue`, `duckFadeDownTime` and
`duckFadeUpTime` hold a separate music duck. Interim rule: duck the music
to `duckValue` under a DJ line with those fades. The captures under the two
DJ lines did not settle it, because the line and the new track's intro
overlap.

## HUD (item 4)

A station change shows a popup at the right of the screen, above the
speedometer:

- a station: its `RS_*` logo (findings "station popup logos"), and under it the
  current track's title (white) and artist (on a pink label), from the
  playlist's `name` and `artist`, right-aligned;
- off: a muted-speaker icon (`RS_Off`) and RADIO OFF on one line, in
  the title's place.

Layout at 1280x720 (measured by eye on captures; scale with the screen):

| Part | x | y |
| --- | --- | --- |
| Logo | 1075 to 1210 | 368 to 412 |
| Title line | right edge near 1207 | about 418 to 442 |
| Artist line | right edge near 1207 | about 446 to 470 |
| Speedometer (for reference) | 1035 to 1210 | 510 to 665 |

Timing, in route frames (60 a second of real time) after the press; the
game rendered about 50 frames a second in these hidden runs (median of the
run's performance log):

| Popup | Starts | Full | Fades out | Gone |
| --- | --- | --- | --- | --- |
| RADIO OFF | before 20 | by 20 | about 185 | by 200 |
| A station | 36 to 48 | 54 to 100 | about 190 | 220 to 240 |

In real time the popup showed for about 2.5 s with fades of about 0.2 s,
and a station's appeared about 0.7 s after the press, probably once the
track's name is known. **The game's popup timer is not real time:** it
steps a fixed 1/30 s each rendered frame (findings "other constant-step
updaters"). At 50 frames a second the 2.5 s are about 125 updates, so on
the console at 30 frames a second the popup would show for about 4.2 s,
and a station's would appear about 1.1 s after the press (if the delay
is counted the same way). Interim rule: show a station's popup for 4 s
after a 1 s delay and RADIO OFF for 4 s at once, fading over 0.2 s
(probable; check by eye in the play test).

`System HUDNewTrackDisplayDelayFreeRoam` and `...Race` are probably the
delay before the same popup shows a new track while listening. That was not
captured. Interim rule: show the popup that long after a track starts.

## State

Per station: the current clip (track, ident or DJ line), the next track,
the position in each playing clip, the recently played lists for
`noRepeat`. For the radio: the selected station (or off), the popup and its
timer, and the duck level. Positions advance only while a station is
selected. Nothing is saved but the selected station (profile option).

## Rates and timing

The music runs on real time, not the simulation step: positions advance in
seconds of audio output. The popup's timer advances a fixed 1/30 s per
rendered frame, so its duration in seconds depends on the frame rate (HUD
section); an engine should time the popup in seconds, using the console's
30 Hz values.

## Unknowns

| Unknown | Confidence of the rule given | Do meanwhile |
| --- | --- | --- |
| The start station in free roam (off once, Rocks once from one seed; run A ended on Rocks, no carried state found) | open | the profile's option, else Bass Arena |
| The popup's console duration (4.2 s from a 1/30 s step at 30 frames a second) and whether its delay is counted the same way | probable | 4 s, 1 s delay |
| Which Rocks track followed `R3_BackOfYourNeck` in run A (`R3_AwayFromHere` or `R3_Animal`) | probable (`AwayFromHere`) | no change to the rules |
| The radio's level under throttle | not measured | set by ear against the idling-engine ratio |
| The 5 s excess on one Bass Arena return (next track under a DJ line) | open | the background rule as given |
| The picker: weights, `noRepeat` window, whether DJ lines follow every track | probable | uniform shuffle with the `noRepeat` window; a DJ line after each ident if it fits before `EventStart` |
| Music ducking under DJ lines (`duckValue`) | probable | duck to `duckValue` with its fade times |
| The station-change fade | probable | `stationChangeFadeTimeUser` out and in |
| The new-track popup | probable | `HUDNewTrackDisplayDelayFreeRoam` after a track starts |
| Whether `identLevel` scales idents | probable | apply it |
| A language without its own VO bank | open | EN |
| Special, immediate and festival DJ lines; interviews; vox pops (`RadioDJVoxPops.xml`) | out of scope for M4 | omit |
| The 3D location stations, the rally radio mode of v4 | out of scope for M4 | omit |

## Tolerances

- Files, counts and name resolution: exact.
- Decoding: each clip's decoded length within one MPEG frame (1,152
  samples) of its header; the 18 checked were exact.
- Cycle order: exact.
- Background rule: a station returned to after t seconds of radio-on time
  is t seconds further on, within 1 s (from the three cases that match the
  rule within 0.2 s).
- Sequencing: ident start at `IdentStart` and next track at
  `OffsetStartNextTrack`, within 0.1 s.
- Popup: delay and duration within 0.3 s of the interim values at 30
  frames a second; position within 2 % of the screen.
- Levels: by ear in the play test, with the radio about 20 dB over the
  idling engine.

## Tests

**Counts** (`tools/re/radio.py census`, disc, EN):

- Banks: music 127 samples (69 stereo, 58 mono, all MPEG at 48 kHz); VO EN
  536. VO banks on the disc: 4; VO info files: 15.
- Radio1 22 tracks, 11 idents, 72 regular DJ lines; Radio2 23, 10, 65;
  Radio3 22, 14, 79; each 29 special, 21 immediate, 10 + 10 festival lines.
- Off: 2 blank tracks, 6 blank idents. Location stations: 21, 8, 11, 11 and
  5 tracks.
- 125 playlist entries, all resolved; 2 music and 47 EN VO samples unused.
- Decode: 18 of 18 samples to their exact length.

**Oracle checks** (`tools/re/radio.py match` on a capture of the engine's
own output, or on the two runs' captures for the game):

- Run A: D-pad right five times from off gives Bass Arena, Pulse, Rocks,
  off, Bass Arena; left twice gives off, Rocks.
- Run B: off for 10 s and back resumes the same track within 0.1 s; 20 s
  of another station while away moves the station on by 20 s.
- A track's ident starts at its `IdentStart`: `R1_ThePower` at 239.9 s
  into the track, `R123_IdentShared_01` from there, and the next track
  10.9 s into the ident.

**Synthetic rule checks** (no game data):

- Stations A and B: A selected for 30 s, then off for 60 s, then B for
  20 s, then A again. A is 20 s past where it was left. B is 50 s past where
  it stood when the radio first came on.
- A track of 200 s with `IdentStart` at 198 s, an ident of 8 s with
  `OffsetStartNextTrack` 6 s: the ident plays from 198 to 206 s, the next
  track starts at 204 s, and the first track ends at 200 s.
