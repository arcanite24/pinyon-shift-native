// DriveSession's side of the first-time career: feeds race::FirstTimeCareer
// (through CampaignDirector) the frame's inputs and applies the commands it
// returns that CampaignAction does not carry (time of day and its lock,
// money, the achievement, the map, cutscene car placement). Car swaps,
// teleports, sat-nav, event launches and the Autoshow go through
// apply_campaign_action like the rest of the campaign.
#include <algorithm>
#include <cctype>
#include <cmath>

#include "drive_session.h"

namespace carview {

void prime_world_and_snap_vehicle(DriveSession& s, std::array<double, 3> target_pos, double target_heading);

namespace {

bool iequal(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
         });
}

}  // namespace

void DriveSession::feed_first_time_career() {
  auto& c = app.campaign;
  if (!c.ftc_active()) return;
  CampaignDirector::FtcFrameInput in;
  const drive::Vec3 vp = vehicle.position();
  in.player_pos = {vp.x, vp.y, vp.z};
  in.throttle = ftc_throttle;
  in.screen_open = map_open || autoshow.is_open() || app.pending_autoshow_open;
  in.loading = loading_screen.active() || host.showroom.has_pending_world();
  c.set_ftc_frame_input(in);
}

void DriveSession::apply_first_time_career() {
  auto& c = app.campaign;
  std::array<double, 3> pos{};
  double heading = 0.0, speed = 0.0;
  if (c.take_place_car(pos, heading, speed)) {
    // A cutscene's PlacePlayerCar: the car on the road there, moving at its
    // initial speed along its heading.
    prime_world_and_snap_vehicle(*this, pos, heading);
    if (speed > 0.0) {
      const drive::Vec3 f = drive::rotate(vehicle.orientation(), {0, 0, 1});
      vehicle.set_velocity({f.x * speed, f.y * speed, f.z * speed}, {0, 0, 0});
    }
  }
  {
    const drive::Vec3 vp = vehicle.position();
    const drive::Vec3 vf = drive::rotate(vehicle.orientation(), {0, 0, 1});
    if (c.take_stream_anchor({vp.x, vp.y, vp.z}, std::atan2(vf.x, vf.z), pos, heading))
      prime_world_and_snap_vehicle(*this, pos, heading);
  }
  for (const auto& cmd : c.take_ftc_commands()) {
    if (cmd.set_time_of_day) {
      app.tod_clock.tod_sec = std::fmod(std::max(0.0, cmd.time_of_day_s), 86400.0);
      if (traffic) traffic->set_hour(app.tod_clock.hour());
    }
    if (cmd.set_time_locked) app.tod_clock.paused = cmd.time_locked;
    if (cmd.give_money) app.career.credits += cmd.give_money;
    if (!cmd.achievement.empty()) {
      // ACHIEVEMENT_HORIZON_ROOKIE: the achievement list names it "Horizon Rookie".
      for (uint16_t id = 1; id < 512; ++id) {
        const auto* e = achievements.find(id);
        if (e && iequal(e->name, "Horizon Rookie")) {
          achievements.unlock(id);
          break;
        }
      }
    }
    if (cmd.open_map) open_map();
    if (cmd.show_satnav_hint) {
      notice = "SAT-NAV: SET A DESTINATION ON THE MAP";  // interim text (Q62)
      notice_until = vehicle.time() + 5;
    }
  }
}

}  // namespace carview
