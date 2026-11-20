# Spec: engine sound for the M3 drive

| | |
| --- | --- |
| Spec ID | `SPEC-ENGINE-SOUND` |
| Status | draft: the file chain, bank format, loop crossfade, pitch and parameter curves confirmed; the rpm map and the loop index rule probable; the scale of the throttle and torque inputs, the DSP stages and the per-view levels probable or open; no audio capture check yet |
| Written | 2026-11-20, specification role ([CLEAN_ROOM.md](../CLEAN_ROOM.md)) |
| Records | [audio findings](../../site/findings/audio.toml), groups "Engine sound" and "Audio engine"; [content findings](../../site/findings/content.toml), group "Cars" ("a car's wheels and sound tuning by media name") |
| Implements it | `shift-engine`, audio module (not yet) |

## What it is

Plan item M3 ([REIMPLEMENTATION_PLAN.md](../REIMPLEMENTATION_PLAN.md))
needs a car that sounds like itself while it is driven: its own recording,
following engine rpm, louder and brighter under load. This spec says where
a car's engine sound lives in the player's files, what a decoder must
handle, which loops play at a given rpm and how loud, and what the mix
adds. A bit-exact mix is not the goal. Where the exact rule was not traced,
the spec says so and gives a default.

A car's engine is not an FMOD event. The game reads a set of loops, each
recorded at one steady rpm, straight from an FSB4 bank and crossfades and
pitches them itself, every audio update. Turbo, supercharger, transmission
whine, exhaust noise, the engine's low-frequency channel, damage and the
electric motor are FMOD Ex event projects: 13 `.fev` projects with their
banks under `media/audio/cars` (also active aero, horns, reflections,
suspension, tyres and wind). Backfires and burbles are 6 sequence banks in
`Engines/Soundbanks/Burbles`, named by the harmonic tuning's `Burble`
element. This spec does not cover them. **M3 needs only the engine
loops.**

Inputs, all from data the engine already has:

- the car's `Data_Car.MediaName` (the merged database of
  [SPEC-CAR-PHYSICS](CAR_PHYSICS.md));
- the stock camshaft's `StallRPM` and `RedlineRPM` (SPEC-CAR-PHYSICS,
  "Engine");
- each step's engine rpm, throttle and engine load from the physics module;
- the camera view (chase, bumper or hood, cockpit).

Evidence comes from:

- the tuning files and banks, read-only, with `tools/re/engine-sound.py`
  over the disc, v4 and the 21 installed packages;
- the loader, the parameter evaluator and the loop update, read in Ghidra
  and in the translated code's data flow (findings group "Engine sound");
- RI-0.6's decode check (`tools/re/media-decode-check.py`) for the codec.

This spec quotes names, counts and measured scalars only. The loop tables
and curves are in the player's files and are read at run time.

## Where a car's engine sound lives (item 1)

### The chain

No database column names a car's engine sound. Everything follows from the
car's media name, matched without regard to case:

```text
Data_Car.MediaName
  |
  +--> audio/cars/Engines/EngineTuning.zip    <MediaName>_ET.xml    engine tuning
  |      EngineSettings: audio_rpm_idle, audio_rpm_redline, drivetrain spring, ...
  |      VolumeAdjustments, GlobalCarVolume, IntakeChuffVolume L0..L3
  |      EngineAmbient / EngineIntake / Exhaust: Upgrade L0 [L1 L2 L3] = harmonic tuning names
  |      HarmonicTunings: EmissionGroup0..2 DSP curves
  |
  +--> audio/cars/CarModelTuning.zip          <MediaName>_CMT.xml   model tuning
         Chassis: emitter positions and cones (EngineIntake, EngineAmbient, ExhaustL, ExhaustR, Transmission)
         Listener: Front / Follow / Cockpit / Default levels and 5.1 pans

harmonic tuning name
  --> audio/cars/Engines/HarmonicTuning.zip  <name>             (an _HT.xml)
        Soundbank name = "<bank>.fsb"
        EngineIntake | EngineAmbient | ExhaustL + ExhaustR : Loop records
        DSP: volume, peaking EQ, low-pass, expander, load EQs

<bank>.fsb
  --> audio/cars/Engines/Soundbanks/LOD1/<bank>.fsb             (loose FSB4 file)
```

All paths are under `media/`. A package adds tuning members in its
`Media/DLCZips/<offer>_pri_<n>.zip` under `media/audio/cars/` and its banks
as loose files in `Media/DLCZips/<offer>_pri_<n>/Media/audio/cars/Engines/Soundbanks/LOD1/`.
v4's `media.zip` replaces four engine tunings (`audio/cars/engines/enginetuning/`).
Resolve members with v4 over the packages (by ascending priority) over the
disc. That order is assumed, not traced for these folders; it matters only
for v4's four engine tunings. The archives use the same
LZX and deflate members as the rest of the disc
([FORMAT-CARS](../formats/CARS.md)).

### Emission groups and upgrade levels

An engine sound has up to three **emission groups**, each a loop set from
one bank and its own emitter in the model tuning:

| Group | Engine tuning element | Harmonic tuning element | Bank suffix | Sample names |
| --- | --- | --- | --- | --- |
| 0, intake | `EngineIntake` | `EngineIntake` | `_Int.fsb` | `EngA_<rpm>` |
| 1, engine (ambient) | `EngineAmbient` | `EngineAmbient` | `_EngAmb.fsb` | `EngB_<rpm>` |
| 2 and 3, exhaust left and right | `Exhaust` | `ExhaustL` and `ExhaustR` | `_Exh.fsb` | `Exh0_<rpm>` |

The group numbers are the order the loader and the update keep them in
(probable for 0 and 1, by the model tuning's order; confirmed for the
exhaust). The sample name ends in the loop's recorded rpm, the same number
as its `rpm_sample`.

The engine tuning names a harmonic tuning per upgrade level as attributes
`L0` to `L3` of each group's `Upgrade` element; the engine ambient has `L0`
only. The level is read from the car's audio descriptor when the car loads,
clamped to 3: one level for the intake and one for the exhaust (probable:
the intake and exhaust upgrade levels). **A stock car uses `L0`.**

A harmonic tuning named `NA.xml` or `NA_<group>_HT.xml`, or a bank named
`NA.fsb`, means the group is silent; the loader sets its loop count to 0.

### Resolution over the player's files

From `tools/re/engine-sound.py census` (merged database: 250 `Data_Car`
rows, 240 distinct models; disc, v4 and 21 packages, the community UDLC
package left out):

| Hop | Resolves |
| --- | --- |
| Engine tuning `<MediaName>_ET.xml` | 239 of 240 models (missing: `VW_IECorrado_95`; `VW_Corrado_95` has one) |
| Model tuning `<MediaName>_CMT.xml` | 236 of 240 (missing: `TOY_RAV4Sport_04`, `VOL_XC70_12`, `KEN_T440_12`, `VW_IECorrado_95`) |
| Intake at `L0`: harmonic tuning, then bank | 228 tunings found (11 are `NA.xml`); 210 banks (18 `NA.fsb`) |
| Engine ambient at `L0` | 239 tunings; 224 banks (15 `NA.fsb`) |
| Exhaust at `L0` | 226 tunings found (13 are `NA.xml`); 222 banks (4 `NA.fsb`) |
| Cars with all three groups | 208 |
| Cars with one or two groups | 30: 14 engine ambient only (among them the 250 GTO, Cobra 427, Countach, Veyron Super Sport), 14 exhaust only (traffic models and economy cars, on `Traffic_` tunings), 2 intake and engine |
| Cars with no group | 1 (`NIS_Leaf_11`, electric; its sound is the ElectricMotor event project) |

A car with an engine tuning but missing groups plays the groups it has. A
car without an engine tuning (`VW_IECorrado_95`) has no engine loops; the
engine should fall back to a car with the same `Data_Engine` row, or log
and stay silent.

The four files with no model tuning have no emitter positions or view
levels: use the defaults below.

`audio/cars/Engines/DSPTuning.zip` (399 files) is an older format that the
executable never names; ignore it (findings, negative).

## Audio format (item 2)

Engine banks are FSB4 files, little-endian, with the header layout of the
findings record "FSB4 sample header layout": a 48-byte bank header, one
sample header per sample (an 80-byte base plus an extra block of 16 to 44
bytes on engine banks, a small table of 32-bit values, probably FMOD's XMA
seek table), then the sample data, each sample 32-byte aligned.

Measured over every engine bank (940 files, 12,845 samples, disc and
packages):

| Property | Value |
| --- | --- |
| Codec | XMA (mode bit `0x01000000`) for all 12,845; no MPEG or PCM |
| Channels | mono 10,198, stereo 2,647 (most exhaust banks) |
| Sample rate | 36,000 Hz for 12,267; 48,000 Hz for 482; 24,000 Hz for 49; 32 distinct rates in all (a few odd rates down to about 6.6 kHz) |
| Loop points | every sample loops over its whole length (loop start 0, loop end length − 1) |
| Samples per bank | up to 22; the banks stock cars use hold 3 to 17 |
| Banks used by the 239 stock cars | 306 |

So a decoder must:

- decode XMA2 at 1 or 2 channels and at the rate in the sample header;
- give exactly the declared number of frames, because the loop is the whole
  sample;
- need not seek: no sample loops over a sub-range, so the seek table can be
  ignored.

FFmpeg's `xma2` decoder meets this: RI-0.6 decoded 32 engine-bank samples
(mono and stereo, four banks) to their exact declared lengths, once each
sample is wrapped as RIFF WAVE with an XMA2WAVEFORMATEX header, as
`tools/re/media-decode-check.py` does. The SDK decoder used by Pinyon Shift
agrees with it on these samples (61 to 65 dB) once its output is aligned 384 frames later. **Bake
each sample once to PCM, loop the PCM, and pitch by resampling.** Check
the seam of a baked loop by ear and by the step at the wrap: the first and
last frames of an XMA decode can carry decoder priming. If a seam clicks,
use a crossfade of a few milliseconds at the wrap. That is not the game's
rule but is inaudible.

## Engine loops (item 3)

### What a group holds

Each `Loop` record of a harmonic tuning gives:

| Attribute | Meaning |
| --- | --- |
| `wavebankindex` | the bank sample, see below |
| `rpm_min`, `rpm_max` | the audio rpm range in which the loop sounds (integers, clamped to 0..32,000 on load) |
| `rpm_sample` | the rpm the loop was recorded at (integer) |
| `volume` | the loop's own level |
| `pitch`, `pitchMin`, `pitchMax` | pitch at `rpm_sample`, and the slope scales below and above it |

At most 16 loops a group are kept. Loops are in rising rpm, each range
overlapping its neighbours: the first starts near idle, and the last one's
`rpm_max` is far above any redline. There are no separate idle, limiter,
on-load or off-load samples. Idle is the lowest loop, the limiter is the
physics rpm bouncing on the top loop, and load changes the DSP stages, not
the choice of loop.

The intake bank usually holds one extra sample (`IntakeChuff_*`) that no
loop names. It belongs to the intake chuff, whose level per upgrade level
is the engine tuning's `IntakeChuffVolume`. Leave it out for M3.

**Loop index.** An index below the bank's sample count n is that sample.
Exhaust tunings list `ExhaustL` at indices 0 to n−1 and `ExhaustR` at n to
2n−1 over n stereo samples. An index i ≥ n plays channel i ÷ n (integer
division) of sample i mod n: the left channel for `ExhaustL` and the right
for `ExhaustR`. Of the stock records, 1,845 resolve that way (probable: the
census, not a trace). 65 records in six cars' `ExhaustR` point past a bank of
mono samples. For those, play sample i mod n (the same mono sample as
`ExhaustL`).

### Rpm map

The loops are chosen and pitched by an **audio rpm**, not the physics rpm.
Each audio update maps the engine rpm from physics, r, as

```text
n        = (r − idle) / (redline − idle)
r_audio  = audio_rpm_idle + (audio_rpm_redline − audio_rpm_idle) × n
```

where `audio_rpm_idle` and `audio_rpm_redline` are the engine tuning's
`EngineSettings` attributes (clamped to 32,000) and idle and redline are the
car's physics rpm range. The physics range is copied from the car's audio
descriptor when the car loads; that it holds the stock camshaft's
`StallRPM` and `RedlineRPM` is probable, not traced. n, unclamped, is also
the curves' RPM input (below). The two ranges are the same in 22 of the 239
engine tunings. On the R34 (car 445) they differ (physics 800 to 8,000, audio
800 to 7,800), so 4,000 physics rpm is 3,911 audio rpm.

When the engine tuning's `drivetrain_spring_enabled` is 1 (22 of 239
cars), the game first adds a drivetrain oscillation to r (two spring-damper
states from `drivetrain_springF1`, `drivetrain_damperR1`, `…F2`, `…R2`,
`drivetrain_scalar` and `drivetrain_clamp`). Leave it out for M3. The R34
has it off.

### Loop crossfade and pitch

For a group with loops 0 to k−1 and audio rpm R, loop i sounds while
`rpm_min`ᵢ ≤ R ≤ `rpm_max`ᵢ. Its crossfade gain is:

```text
rising edge   (R < rpm_max of loop i−1):   t = (R − rpm_minᵢ) / (rpm_max₍ᵢ₋₁₎ − rpm_minᵢ)
falling edge  (R > rpm_min of loop i+1):   t = (rpm_maxᵢ − R) / (rpm_maxᵢ − rpm_min₍ᵢ₊₁₎)
gainᵢ = log2(1 + t) on an edge, 1 elsewhere in the range
```

When both edge conditions hold (a loop narrower than its overlaps), the
rising edge wins. The first loop has no rising edge, and the last has no
falling edge (the
game uses 0 and 32,000 as the missing neighbour's edges). log2(1 + t)
rises from 0 to 1. At the middle of an overlap each loop has gain
log2 1.5 = 0.585: the pair sums to 1.17 in amplitude and 0.68 in power,
between a linear and an equal-power fade. This is confirmed from the translated loop update.

Pitch, as a playback-rate ratio:

```text
R ≤ rpm_sampleᵢ:  pitchᵢ + pitchMinᵢ × (R − rpm_sampleᵢ) / rpm_sampleᵢ
R ≥ rpm_sampleᵢ:  pitchᵢ + pitchMaxᵢ × (R − rpm_sampleᵢ) / rpm_sampleᵢ
```

With `pitch` = `pitchMin` = `pitchMax` = 1, which holds for 10,866 of the
10,869 loops stock cars use (3 have another `pitchMax`), this is
R / `rpm_sample`. The ratio applies on top of
the sample's own rate (36 kHz played at an output of 48 kHz is a further
36/48 resample).

The voice's volume is gainᵢ × `volume`ᵢ × the group gain (next section).
When R leaves a loop's range, the game multiplies the voice's volume by
0.2 each audio update and stops it below 0.001. At the game's update rate
of about 30 Hz that is silence in about 0.15 s; an engine should fade a
loop out over about 150 ms.

A voice is started once its volume exceeds 0.001. Whether the game aligns
the phases of neighbouring loops is not traced; the loops are independent
recordings, so starting each at its beginning or at a random offset is fine.

### Parameter curves (load and throttle)

Every gain, filter frequency, EQ gain and expander setting in the engine
and harmonic tunings is one **parameter**: a `PhysicsCoeff` element (RPM,
Throttle, PosTorque and NegTorque, each clamped to 0..1) and a
`ThreePointCurve` element (x0..x2 clamped to 0..1, y0..y2). Each update:

```text
x = RPM × n + Throttle × throttle + PosTorque × posTorque − NegTorque × negTorque
y = piecewise linear through (x0, y0), (x1, y1), (x2, y2); y0 below x0, y2 above x2
```

This is confirmed from the evaluator. The inputs are the normalised rpm n
above, and the throttle, positive torque and negative torque the car's
audio inputs carry. Their scale is not traced. Probable, and the default
for M3:

- throttle is the pedal after input shaping, 0..1;
- posTorque is the engine's load when it drives, 0..1 (the load of
  SPEC-CAR-PHYSICS, "Engine": its positive part);
- negTorque is the load when it brakes. Its sign is open: the game's
  loop audition mode drives all three of throttle, posTorque and
  negTorque at +1, which suggests a magnitude (0..1, lowering x under
  engine braking), while the curves that weight NegTorque rise as if it
  raised x. Make the sign a switch, start with the magnitude, and choose by
  ear on lift-off.

### Which parameters M3 needs

| Parameter | Where | Use for M3 |
| --- | --- | --- |
| Overall engine level | engine tuning `GlobalCarVolume` | yes: multiplies every group |
| Group level | the harmonic tuning's `DSP/Volume/Gain` | yes: the group gain. When the engine tuning has `HarmonicTunings/EmissionGroup<g>/Volume/Gain`, use that instead (probable: the engine tuning is read after the harmonic tuning) |
| Engine volume trims | engine tuning `VolumeAdjustments` (`EngineVolume` and the others) | yes for `EngineVolume`; the rest are for the event-project sounds |
| Low-pass | `DSP/Lowpass` (`CutoffFrequency`, `Resonance`) per group | optional: a one-pole or biquad low-pass at the curve's cutoff, with `Active` = 1 |
| Peaking EQ | `DSP/PEQ` (`Gain` as a linear factor, `CenterFrequency` in Hz, `Bandwidth` in octaves, probable) | later |
| Load EQs | `DSP/LoadPEQ/PosLoad` and `NegLoad` (exhaust) | later; they are what makes the exhaust bark on load. The cheap stand-in for M3 is the group gain, whose curve already rises with throttle |
| Expander | `DSP/Expander` (`MaxGain`, `Settings`), exhaust | later |
| Idle gain reset | the harmonic tuning's `EngineSettings/audio_rpm_idle_gain_reset` | later (its use is not traced) |

With these, the sound follows rpm through the loops and pitch, and load
through the gain curves. That is enough for M3's play test.

## Mix basics (item 4)

- **Per view.** The model tuning's `Listener/Basic` has four views:
  `Front`, `Follow`, `Cockpit` and `Default`. Each has `EmissionGroupVolumes`
  (`EngineIntake`, `EngineAmbient`, `ExhaustL`, `ExhaustR`), `OtherVolumes`
  for the event-project sounds, and a 5.1 pan per emission group
  (`SurroundPan<group>`: `Left`, `Right`, `Center`, `Lfe`, `LeftRear`,
  `RightRear`, and a `Pan3d` blend). The update multiplies each group's
  gain by the current view's emission level (confirmed for the multiply;
  which view is which camera is probable). `Follow`, with its listener
  origin about 4 m behind the car, is the chase camera. `Front` is the
  bumper and hood. `Cockpit` is the interior: on the R34 it lowers the exhaust and
  raises the engine ambient. `Default` is probably other
  cars and replays. Use `Follow` for the chase camera and `Cockpit` inside.
  Without a model tuning, use 1 for every group.
- **Stereo.** Fold each group's 5.1 pan to stereo as Left + 0.7 × Center +
  0.7 × LeftRear for the left channel, and the mirror for the right (our
  fold, not the game's). `Pan3d` = 1 means positional, from the emitter.
  For M3, a fixed pan is fine.
- **Emitters.** `Chassis` gives each group's position in the car frame
  (FORMAT-CARS axes: +z to the rear), an orientation and a cone (`innerAngle`,
  `outerAngle`, `outerVolume`). They matter for outside cameras and
  replays, not for M3's chase view.
- **Category level.** The player's car is probably the `FocusCar` category
  of the mixer snapshots (findings "mixer snapshots multiply"). A fixed
  engine level is enough for M3. Levels from all three files multiply,
  so expect to set one master gain by ear.
- **Cockpit filter.** `CockpitDSPConfig.xml` holds a cockpit EQ with
  `Bypass` = 1 on the disc, so the cockpit is not filtered beyond its
  levels. The engine tuning's `opencockpit` flag
  is set for 30 of the 239 cars and marks open cars. Its effect is
  not traced.
- **Voices.** `AudioEngineConfig.xml` caps the voices per group
  (`maxEngineIntake`, `maxEngineAmbient` and others). With at most two
  loops sounding a group (three during a fast fade-out), one car needs about
  8 to 12 voices.

## State

Per car: the resolved loop sets (up to four groups of at most 16 loops,
each with its baked PCM), per loop a voice (playing or not, volume, phase),
the parameters read for the car, and the view. Per update the inputs are
the physics rpm, throttle and load, and the view.

## Rates and timing

The game updates the car's engine sound once per audio update on its audio
thread, about 28 to 30 times a second in a race (findings
"CAudioEngine::Update"). Gains and pitch change in steps at that rate, and
FMOD's mixer smooths volume over each 256-sample block. An engine may update
at its frame or physics rate. It should smooth gain and pitch over a few
milliseconds to avoid zipper noise, and fade a loop that left its range
over about 150 ms (the game's × 0.2 per update).

## Unknowns

| Unknown | Confidence of the rule given | Do meanwhile |
| --- | --- | --- |
| The physics rpm range of the rpm map (`StallRPM`, `RedlineRPM`) | probable | use them; the capture check below settles it |
| The scale of throttle and posTorque; the sign of negTorque | probable; open for the sign | the defaults above; a switch for the sign |
| Group order 0 and 1 (intake, ambient) | probable | as in the table; both use the same rule |
| The engine tuning's `HarmonicTunings` against the harmonic tuning's `DSP` | probable (engine tuning wins) | as above |
| Which upgrade selects `L1` to `L3` | probable (intake and exhaust upgrades) | `L0` for stock |
| How the game plays the 65 out-of-range `ExhaustR` indices | not traced | reuse the mono sample |
| PEQ units, the expander's law, the load EQs, the idle gain reset | open | leave out for M3 |
| Cameras for `Front` and `Default`, the effect of `opencockpit` | probable or open | `Follow` and `Cockpit` |
| Turbo, supercharger, transmission, exhaust noise, burbles and backfires, the engine LFE, damage, the electric motor | out of scope: FMOD event projects, `.fev` (RF-6) | omit for M3 |
| `VW_IECorrado_95`, which has no engine tuning | not traced | fall back or stay silent, and log |

## Tolerances

- Loop choice and crossfade gains: exact (they are a rule on file values).
- Pitch: the ratio within 0.1 % (a tenth of the step the ear notices).
- Levels: by ear in the play test; the game's absolute levels come from
  three multiplied sources, so only relative levels across rpm matter for M3.
- Against a capture (when one exists): the firing frequency of the engine
  loop within 2 % of the oracle at the same physics rpm.

## Tests

**Counts** (`tools/re/engine-sound.py census`, merged database, disc, v4 and
21 packages):

- 240 car models; 239 resolve an engine tuning, 236 a model tuning.
- At `L0`: intake 210 banks, engine ambient 224, exhaust 222; 208 cars with
  all three groups, 30 with one or two, 1 with none.
- Silent markers at `L0`: intake 11 `NA.xml` and 18 `NA.fsb`, engine
  ambient 15 `NA.fsb`, exhaust 13 `NA.xml` and 4 `NA.fsb`.
- 940 engine banks, 12,845 samples, all XMA, all whole-sample loops; 10,198
  mono, 2,647 stereo; 306 banks used by stock cars.
- Loop sets used by stock cars: 3 to 16 loops; 1,845 loop indices on a
  second channel, 65 past a mono bank.

**Synthetic rule checks** (no game data):

- Two loops with ranges 1,000 to 2,200 and 2,000 to 3,000 rpm: at 2,100 rpm
  t = 0.5 for both, so each has gain 0.585. At 1,500 the first plays alone
  at gain 1. At 2,000 the second's gain is 0 and the first's 1.
- A loop with `rpm_sample` 3,000 and `pitch` = `pitchMin` = `pitchMax` = 1
  plays at ratio 1.2 at 3,600 rpm and 0.8 at 2,400. With `pitchMax` 0.5
  it plays at 1.1 at 3,600.
- A parameter with coefficients (RPM 0.5, Throttle 0.5, 0, 0) and points
  (0, 0.2), (0.5, 0.5), (1, 1): at n = 0.4 and throttle 1, x = 0.7 and
  y = 0.7.

**The R34 at 4,000 rpm, full load** (car 445, stock, `L0`;
`tools/re/engine-sound.py loops --car 445 --rpm 4000`). These rows assume
the rpm map's physics range is `StallRPM` to `RedlineRPM` (probable, see
Unknowns); they change if it is not:

| Group | Loops sounding (of the set) | Crossfade gain | Pitch ratio |
| --- | --- | --- | --- |
| Intake (13 loops) | 5 and 6 | 0.596 and 0.574 | 1.111 and 0.908 |
| Engine ambient (12) | 5 alone | 1 | 1.025 |
| Exhaust left and right (12 each) | 5 alone | 1 | 1.025 |

The audio rpm there is 3,911. Without the rpm map (audio rpm = physics rpm)
the exhaust and engine would instead crossfade loops 5 and 6 at 0.880
and 0.214. So this case also tells whether an implementation applies the
map. At 7,000 physics rpm (audio 6,828, under the same assumption) the engine
and exhaust crossfade loops 10 and 11 at 0.523 and 0.644, and the intake
plays loop 11 alone.

**Pending, measured check** (needs a game run). The map is pinned at its
ends: physics idle gives `audio_rpm_idle` and physics redline gives
`audio_rpm_redline`. Steady tones there settle the physics range without
aligning a trace. With every mix category but the engine at 0 and
`audiocapture`:

- hold the R34 on the limiter in neutral or first gear. Its exhaust
  fundamental (the RB26 fires three times a revolution, so rpm / 20 Hz)
  should sit at the audio redline, 7,800 / 20 = 390 Hz, under the map,
  against 400 Hz if the game used the physics rpm;
- let a car idle whose `audio_rpm_idle` differs from its `StallRPM`
  (106 of the 239 cars; car 365 differs by 450 rpm) and compare the
  idle tone the same way.

A full-throttle sweep cannot decide it: at 4,000 rpm the two readings
differ by 2.2 %, about the tolerance.
