#include "focus_audio_state.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "audio_host.h"
#include "drive/lamps.h"
#include "drive_helpers_runtime.h"

namespace carview {

uint8_t collision_surface_hint(const world::Collision& col, int surf_idx) {
  const auto& surfs = col.surfaces();
  if (surf_idx < 0 || size_t(surf_idx) >= surfs.size()) return 0;
  const auto& sf = surfs[size_t(surf_idx)];
  const std::string name = lower_ascii(sf.name);
  const std::string cat = lower_ascii(sf.category);
  if (cat == "guardrail" || name.find("guardrail") != std::string::npos || name.find("metal") != std::string::npos ||
      name.find("steel") != std::string::npos || name.find("rail") != std::string::npos) {
    return 3;  // Metal/Guardrail
  }
  if (name.find("wood") != std::string::npos || name.find("timber") != std::string::npos ||
      name.find("plank") != std::string::npos || name.find("fence") != std::string::npos) {
    return 4;  // Wood/Prop
  }
  if (cat == "softworld" || sf.off_roadness >= 0.25f || name.find("dirt") != std::string::npos ||
      name.find("grass") != std::string::npos || name.find("gravel") != std::string::npos ||
      name.find("sand") != std::string::npos || name.find("mud") != std::string::npos ||
      name.find("snow") != std::string::npos) {
    return 2;  // Dirt/Grass
  }
  return 0;  // Asphalt/Concrete
}

void fill_focus_audio_state(const drive::Vehicle& vehicle, const audio::EnginePlayer::Update& engine_in,
                            const world::Collision& collision, const AudioHost& host,
                            audio::VehicleAudioState& vst) {
  const auto& vp = vehicle.params();
  const drive::Vec3 vpos = vehicle.position();
  const drive::Vec3 vvel = vehicle.velocity();
  const drive::Vec3 vright = drive::rotate(vehicle.orientation(), {1, 0, 0});
  vst.speed_mph = float(std::fabs(vehicle.speed()) * 2.2369363);
  vst.top_speed_mph = float(
      std::clamp(double(drive::CockpitAnimState::estimate_dial_max_speed_mps(vp)) * 2.2369363, 90.0, 280.0));
  vst.rpm = float(vehicle.rpm());
  vst.idle_rpm = float(vp.idle_rpm);
  vst.redline_rpm = float(vp.redline_rpm);
  vst.throttle = float(vehicle.engine_throttle());
  vst.engine_load = float(std::clamp(std::max(engine_in.pos_torque, std::fabs(engine_in.neg_torque)), 0.0, 1.0));
  vst.gear = vehicle.gear();
  if (vst.gear < 0) {
    vst.gear_ratio = float(std::fabs(vp.reverse_gear * vp.final_drive));
  } else if (vst.gear >= 1 && size_t(vst.gear - 1) < vp.gears.size()) {
    vst.gear_ratio = float(std::fabs(vp.gears[size_t(vst.gear - 1)] * vp.final_drive));
  }
  vst.shifting = vehicle.shifting();
  vst.clutch_pos = float(1.0 - vehicle.clutch());
  vst.forced_induction = vp.has_turbo ? 1 : (vp.has_supercharger ? 2 : 0);
  vst.turbo_psi = float(vehicle.boost_psi());
  vst.turbo_norm_rpm = vp.has_turbo && vp.boost_max_psi > 1.0
                           ? float(std::clamp(vehicle.boost_psi() / vp.boost_max_psi, 0.0, 1.2))
                           : 0.0f;
  vst.bov_open = vehicle.bov_open();
  vst.supercharger_norm_rpm = vp.has_supercharger && vp.redline_rpm > 1.0
                                  ? float(std::clamp(vehicle.rpm() / vp.redline_rpm, 0.0, 1.2))
                                  : 0.0f;
  const auto& body_c = vehicle.strongest_body_contact();
  vst.scraping = body_c.force > 150.0 && vst.speed_mph > 4.0f;
  vst.scrape_intensity = vst.scraping ? float(std::clamp(body_c.force / 20000.0, 0.2, 1.0)) : 0.0f;
  const auto& ws = vehicle.wheels();
  const auto& surfs = collision.surfaces();
  if (vst.scraping && body_c.surface >= 0 && size_t(body_c.surface) < surfs.size()) {
    const std::string sn = lower_ascii(surfs[size_t(body_c.surface)].name);
    if (sn.find("metal") != std::string::npos || sn.find("guard") != std::string::npos ||
        sn.find("steel") != std::string::npos) {
      vst.scrape_surface_hint = 3;
    } else if (sn.find("wood") != std::string::npos || sn.find("tree") != std::string::npos) {
      vst.scrape_surface_hint = 4;
    } else if (sn.find("dirt") != std::string::npos || sn.find("grass") != std::string::npos ||
               sn.find("gravel") != std::string::npos || sn.find("mud") != std::string::npos) {
      vst.scrape_surface_hint = 2;
    }
  }
  for (int wi = 0; wi < 4; ++wi) {
    auto& wa = vst.wheels[size_t(wi)];
    const auto& wstate = ws[size_t(wi)];
    wa.slip_ratio = float(std::fabs(wstate.slip_ratio));
    wa.slip_angle = float(std::fabs(wstate.slip_angle));
    wa.slip_combined = float(std::hypot(wstate.slip_ratio, wstate.slip_angle));
    wa.wheel_rpm = float(wstate.omega * 9.5492966);
    const double sload = std::max(100.0, vehicle.static_load(wi));
    wa.load = wstate.contact ? float(std::clamp(wstate.load / sload, 0.0, 2.0)) : 0.0f;
    const int axle = wi < 2 ? 0 : 1;
    if (wstate.contact && wstate.compression > vp.bump_travel[size_t(axle)] * 0.92)
      wa.bottom_out_force = float(wstate.compression - vp.bump_travel[size_t(axle)] * 0.92);
    uint8_t sgrp = 0;
    int16_t sidx = -1;
    if (wstate.surface >= 0 && size_t(wstate.surface) < surfs.size()) {
      const auto& sf = surfs[size_t(wstate.surface)];
      if (!sf.name.empty()) {
        sgrp = host.map_tire_surface_group(sf.name);
        sidx = host.map_tire_surface_index(sf.name);
      } else if (sf.off_roadness > 0.5f) {
        sgrp = 2;
      } else if (sf.off_roadness > 0.05f) {
        sgrp = 1;
      }
    }
    wa.surface_group = sgrp;
    wa.surface_index = sidx;
  }
  vst.listener_pos = {float(vpos.x), float(vpos.y), float(vpos.z)};
  vst.listener_right = {float(vright.x), float(vright.y), float(vright.z)};
  vst.listener_vel = {float(vvel.x), float(vvel.y), float(vvel.z)};
}

}  // namespace carview
