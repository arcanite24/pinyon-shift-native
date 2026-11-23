---
title: Features
section: Start
order: 3
description: Subsystems implemented in Shift Engine: driving physics, audio, Colorado world, career and DLC, UI4, wheels, DualSense, OpenTrack, Photo Mode, replays and achievements.
---

# Features

Shift Engine reimplements the core gameplay, rendering, audio and progression systems in native C++20 while reading assets and tuning tables directly from your disc.

## Driving physics and assists

Car physics run at a fixed 120 Hz simulation rate decoupled from the display refresh rate (`libs/drive`):

- **Per-car database parameters:** Mass, inertia, drivetrain, torque curves, gear ratios, forced-induction boost lag, brake torque and bias, aerodynamic drag and downforce, axle differentials, suspension geometry and upgrade parts are read from your disc's `gamedb.slt` and baked into `.shiftbake` files.
- **Surface physics:** Asphalt, dirt, gravel, grass and water respond with surface friction, bumpiness and damping rules derived from Colorado's collision geometry.
- **Transmission and assists:** Automatic, Manual and Manual+Clutch shifting; toggleable ABS, traction control (TCS), stability management (STM), steering assist and auto-brake, with difficulty reward bonuses.
- **3D driving line and rewind:** Full or braking-only 3D racing line projected onto the road surface, and a rolling rewind ring buffer with rewind camera playback.
- **AI drivers:** Multi-car race AI and road traffic running on a persistent worker pool, with options for clean racing (rubber-banding off) and adjustable opponent difficulty.

## Audio and radio

Audio runs on a dedicated lock-free thread (`libs/audio`, `apps/carview/audio_host.cpp`):

- **Engine synthesis:** `soundbake` decodes your disc's FSB4/XMA2 engine loops to PCM once during setup (when FFmpeg is installed). At runtime the engine reads each car's engine, harmonic and listener XML curves to crossfade loops by RPM, load and throttle, plus tyre, wind, impact and UI cues.
- **Radio stations:** Reads `RadioSystem.xml` and `RadioSoundbankInfo` to schedule **Bass Arena**, **Horizon Pulse** and **Horizon Rocks** on a shared clock, streaming MPEG tracks via `dr_mp3` with DJ voiceover and station popups.
- **Custom radio and mix controls:** Separate volume levels for engine, other cars, tyres, wind, ambience, voiceover and music, plus your own music tracks packed through the launcher's [Custom Radio Station Builder](modding.md#custom-radio-station-builder).

## Colorado world, career and DLC

- **Open world streaming:** Background workers stream Colorado's terrain tiles, props, road network, kind-7 billboard trees, collision meshes, smashable objects and far-ring UberLOD geometry without stalling the render thread.
- **Time of day and lighting:** Continuous 24-hour sky, sun/moon position, stars, cascaded sun shadows, dynamic environment cubemaps, car headlights and backlit interior instrument gauges.
- **Free roam and progression:** Road discovery, discount signs, barn finds, speed traps and speed zones, skill chains, sponsor challenges, popularity levels, manufacturer affinity, outposts, street races, PR stunts and showcases.
- **Garage, Autoshow and tuning:** 240+ disc cars (plus imported DLC cars), Autoshow manufacturer and color selection, My Garage with instant hot car swaps (~0.21 s), performance upgrades, PI recalculation, tuning, visual parts, rims and the livery/paint editor.
- **DLC, Title Update v4, Rally and 1000 Club:** Verify and import your own Xbox 360 STFS packages (`CON`/`LIVE`/`PIRS`) or extracted offer folders in `shift-launcher`. With your Title Update v4 and expansion files mounted, **Horizon Rally** (rally stages and co-driver pace notes) and offline **1000 Club** medal challenges are supported.

## UI4 interface and overlays

Shift Engine parses the disc's Anark UI4 scene graphs (`.bgf`, `.bsg`, `.fbf`), `.xds` masks and artwork, and all 11 vector fonts (`fontmap.xml`) directly:

- **Authentic tilted chrome:** Menus, HUD, pause menu, Autoshow, garage, event hubs, map and Shift Engine's own utility screens share the game's tilted blade layout and vector typography, scaled for 16:9 and ultrawide displays.
- **Live settings (<kbd>F6</kbd>):** Change graphics presets, shadows, MSAA, SSAO, FXAA, bloom, upscaling, HDR10 calibration, audio mix, assists, difficulty and camera options without restarting.
- **Trainer (<kbd>F10</kbd>):** Separate cheat profile for credits, wristbands, time of day, game speed, free camera and testing tools so career saves stay untouched.
- **Frame inspector and telemetry overlay (<kbd>F3</kbd>):** Live frame-time, CPU/GPU pass breakdown, VRAM/RSS and draw-count telemetry.

## Controllers, wheels, DualSense and head tracking

- **Gamepads and keyboard:** Native SDL3 controller support (Xbox, PlayStation, Nintendo, Steam Input, generic USB/BT gamepads) with rumble and rebindable layouts, plus full keyboard driving and Android multi-touch HUD.
- **Racing wheels:** Steering, throttle, brake and clutch axis mapping with deadzones, saturation, linearity curves and force-feedback tuning.
- **DualSense:** Adaptive trigger resistance (throttle/brake feedback) and lightbar integration over USB.
- **Head tracking and UDP telemetry:** OpenTrack UDP head-tracking input for cockpit and hood cameras, plus outbound Forza-style UDP physics telemetry for external dashboards and motion rigs.

## Photo Mode, replays and achievements

- **Photo Mode (<kbd>P</kbd>) and Horizon Promo:** Free orbit camera with aperture, shutter, exposure and filter controls, PNG export (`stb_image_write`) and Horizon Promo car-capture tracking.
- **Replays:** Rolling race and free-roam replay capture with playback, camera switching and scrub controls.
- **Achievements (<kbd>F7</kbd>):** Reads your disc's SPA achievement definitions and 64×64 icons, tracks progress bars and unlock toasts offline, and groups achievements by wristband tier and category.
