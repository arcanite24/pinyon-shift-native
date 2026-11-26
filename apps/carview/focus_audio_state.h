#pragma once

#include "audio/engine_player.h"
#include "audio/sfx_engine.h"
#include "drive/vehicle.h"
#include "world/collision.h"

namespace carview {

class AudioHost;

// The focus car's part of the vehicle audio state that follows from the
// physics and the ground alone: speed, rpm, throttle and load, gear and
// clutch, forced induction, the body's scrape and each wheel's slip, load
// and tyre surface. The listener is put at the car. The windowed loop adds
// the camera, upgrades, damage and the UI state on top; headless renders use
// it as it is, so the tyres, wind and surfaces sound in an --audio-out run.
void fill_focus_audio_state(const drive::Vehicle& vehicle, const audio::EnginePlayer::Update& engine_in,
                            const world::Collision& collision, const AudioHost& host,
                            audio::VehicleAudioState& vst);

// The impact sound's surface class for a collision surface: 0 asphalt,
// 2 dirt or grass, 3 metal or guardrail, 4 wood.
uint8_t collision_surface_hint(const world::Collision& col, int surf_idx);

}  // namespace carview
