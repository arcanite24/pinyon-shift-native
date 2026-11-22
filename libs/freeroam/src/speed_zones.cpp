#include "freeroam/speed_zones.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include "audio/xml.h"
#include "fh1data/archive.h"

namespace freeroam {
namespace {

constexpr double kMsToMph = 2.2369362920544;

bool fail(std::string* error, const std::string& what) {
  if (error) *error = what;
  return false;
}

std::string lower_str(std::string_view s) {
  std::string out(s);
  for (char& c : out)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return out;
}

SpeedGate make_gate(const std::array<double, 3>& l, const std::array<double, 3>& r) {
  SpeedGate g;
  g.left = l;
  g.right = r;
  g.center = {0.5 * (l[0] + r[0]), 0.5 * (l[1] + r[1]), 0.5 * (l[2] + r[2])};
  g.width = std::hypot(r[0] - l[0], r[2] - l[2]);
  return g;
}

// Segment-segment intersection in XZ between motion (x0,z0)->(x1,z1) and the
// gate segment (left -> right) padded by `pad` m on each side so cars on wide
// verges still trigger cleanly.
bool crossed_gate(double x0, double z0, double x1, double z1, const SpeedGate& gate, double pad = 4.5) {
  const double gx = gate.right[0] - gate.left[0];
  const double gz = gate.right[2] - gate.left[2];
  const double gl = std::hypot(gx, gz);
  if (gl < 0.5) return false;
  const double ux = gx / gl, uz = gz / gl;
  const double ax = gate.left[0] - ux * pad, az = gate.left[2] - uz * pad;
  const double bx = gate.right[0] + ux * pad, bz = gate.right[2] + uz * pad;

  const double rx = x1 - x0, rz = z1 - z0;
  const double sx = bx - ax, sz = bz - az;
  const double denom = rx * sz - rz * sx;
  if (std::fabs(denom) < 1e-9) return false;
  const double qpx = ax - x0, qpz = az - z0;
  const double t = (qpx * sz - qpz * sx) / denom;
  const double u = (qpx * rz - qpz * rx) / denom;
  return t >= 0.0 && t <= 1.0 && u >= 0.0 && u <= 1.0;
}

std::string pretty_speed_name(const std::string& id, bool is_zone) {
  // Format "speed_camera_01" -> "SPEED TRAP 01", "average_speed_05" -> "SPEED ZONE 05".
  std::string num;
  for (size_t i = id.size(); i-- > 0;) {
    if (id[i] >= '0' && id[i] <= '9') num.insert(num.begin(), id[i]);
    else if (!num.empty()) break;
  }
  return (is_zone ? "SPEED ZONE " : "SPEED TRAP ") + (num.empty() ? id : num);
}

}  // namespace

const SpeedTrapDef* SpeedActivitiesData::find_trap(std::string_view id) const {
  for (const auto& t : traps)
    if (audio::iequals(t.id, id)) return &t;
  return nullptr;
}

const SpeedZoneDef* SpeedActivitiesData::find_zone(std::string_view id) const {
  for (const auto& z : zones)
    if (audio::iequals(z.id, id)) return &z;
  return nullptr;
}

bool parse_speed_activities(std::string_view speed_camera_xml, std::string_view average_speed_xml,
                            std::string_view gameobjs_xml, SpeedActivitiesData& out, std::string* error) {
  out = SpeedActivitiesData{};
  audio::XmlNode cam_xml, avg_xml, objs_xml;
  std::string why;
  if (!audio::parse_xml(speed_camera_xml, cam_xml, &why)) return fail(error, "speed_camera.xml: " + why);
  if (!audio::parse_xml(average_speed_xml, avg_xml, &why)) return fail(error, "average_speed.xml: " + why);
  if (!audio::parse_xml(gameobjs_xml, objs_xml, &why)) return fail(error, "GameObjs.xml: " + why);

  std::map<std::string, std::array<double, 3>> objs;
  for (const auto& obj : objs_xml.children) {
    const std::string gid = lower_str(obj.attr("GameplayID"));
    if (gid.empty()) continue;
    if (gid.rfind("speed_camera_", 0) != 0 && gid.rfind("average_speed_", 0) != 0) continue;
    if (const auto* pos = obj.child("Pos")) {
      objs[gid] = {pos->number("x", 0.0), pos->number("y", 0.0), pos->number("z", 0.0)};
    }
  }

  for (const auto& act : cam_xml.children) {
    if (!audio::iequals(act.name, "Activity") || !audio::iequals(act.attr("type"), "ActivitySpeedCamera")) continue;
    const std::string id = act.attr("name");
    const std::string lid = lower_str(id);
    const auto il = objs.find(lid + "_left");
    const auto ir = objs.find(lid + "_right");
    if (il == objs.end() || ir == objs.end()) continue;
    SpeedTrapDef t;
    t.id = id;
    t.scoreboard_id = int(act.number("scoreboard_id", 0));
    if (const auto* thr = act.child("SpeedThreshold")) t.min_speed_mph = thr->number("minimumSpeed", 30.0);
    t.gate = make_gate(il->second, ir->second);
    out.traps.push_back(std::move(t));
  }

  for (const auto& act : avg_xml.children) {
    if (!audio::iequals(act.name, "Activity") || !audio::iequals(act.attr("type"), "ActivityAverageSpeed")) continue;
    const std::string id = act.attr("name");
    const std::string lid = lower_str(id);
    const auto il1 = objs.find(lid + "_left1");
    const auto ir1 = objs.find(lid + "_right1");
    const auto il2 = objs.find(lid + "_left2");
    const auto ir2 = objs.find(lid + "_right2");
    if (il1 == objs.end() || ir1 == objs.end() || il2 == objs.end() || ir2 == objs.end()) continue;
    SpeedZoneDef z;
    z.id = id;
    z.scoreboard_id = int(act.number("scoreboard_id", 0));
    if (const auto* thr = act.child("SpeedThreshold")) z.min_speed_mph = thr->number("minimumSpeed", 30.0);
    z.gate1 = make_gate(il1->second, ir1->second);
    z.gate2 = make_gate(il2->second, ir2->second);
    z.midpoint = {0.5 * (z.gate1.center[0] + z.gate2.center[0]), 0.5 * (z.gate1.center[1] + z.gate2.center[1]),
                  0.5 * (z.gate1.center[2] + z.gate2.center[2])};
    out.zones.push_back(std::move(z));
  }

  return !out.traps.empty() || !out.zones.empty();
}

bool load_speed_activities(const std::filesystem::path& game_root, SpeedActivitiesData& out, std::string* error) {
  std::string why;
  auto zip = fh1::Archive::open(game_root / "media" / "gamemodes.zip", &why);
  if (!zip) return fail(error, why);
  auto cam = zip->read("Colorado/speed_camera.xml");
  if (!cam.ok()) return fail(error, "Colorado/speed_camera.xml: " + cam.error);
  auto avg = zip->read("Colorado/average_speed.xml");
  if (!avg.ok()) return fail(error, "Colorado/average_speed.xml: " + avg.error);

  std::ifstream go(game_root / "media" / "tracks" / "colorado" / "Ribbon_00" / "GameObjs.xml", std::ios::binary);
  if (!go) return fail(error, "cannot open Ribbon_00/GameObjs.xml");
  std::stringstream ss;
  ss << go.rdbuf();
  const std::string objs_str = ss.str();

  auto sv = [](const std::vector<uint8_t>& d) { return std::string_view(reinterpret_cast<const char*>(d.data()), d.size()); };
  return parse_speed_activities(sv(cam.data), sv(avg.data), objs_str, out, error);
}

SpeedActivityTracker::SpeedActivityTracker(const SpeedActivitiesData& data) : data_(data) {}

void SpeedActivityTracker::load_records(const std::vector<std::pair<std::string, double>>& traps,
                                        const std::vector<std::pair<std::string, double>>& zones) {
  best_traps_.clear();
  best_zones_.clear();
  for (const auto& [id, mph] : traps)
    if (mph > 0.0) best_traps_[id] = std::max(best_traps_[id], mph);
  for (const auto& [id, mph] : zones)
    if (mph > 0.0) best_zones_[id] = std::max(best_zones_[id], mph);
}

std::vector<std::pair<std::string, double>> SpeedActivityTracker::save_traps() const {
  return {best_traps_.begin(), best_traps_.end()};
}

std::vector<std::pair<std::string, double>> SpeedActivityTracker::save_zones() const {
  return {best_zones_.begin(), best_zones_.end()};
}

double SpeedActivityTracker::best_trap_mph(const std::string& id) const {
  const auto it = best_traps_.find(id);
  return it == best_traps_.end() ? 0.0 : it->second;
}

double SpeedActivityTracker::best_zone_mph(const std::string& id) const {
  const auto it = best_zones_.find(id);
  return it == best_zones_.end() ? 0.0 : it->second;
}

void SpeedActivityTracker::reset_session_clock() {
  trap_cd_.clear();
  zone_cd_.clear();
  active_zone_idx_ = -1;
  entry_gate_ = 0;
  zone_elapsed_ = zone_dist_ = zone_slow_t_ = 0.0;
  banner_at_ = flash_at_ = -1e9;
}

std::vector<SpeedActivityEvent> SpeedActivityTracker::step(double dt, double time, double x0, double z0, double x1,
                                                           double z1, double speed_mps) {
  std::vector<SpeedActivityEvent> out;
  if (dt <= 0.0) return out;
  const double mph = std::fabs(speed_mps) * kMsToMph;
  const double moved = std::hypot(x1 - x0, z1 - z0);

  // 1. Check Speed Traps (single gate crossing at >= minimumSpeed).
  for (const auto& t : data_.traps) {
    if (std::hypot(x1 - t.gate.center[0], z1 - t.gate.center[2]) > t.gate.width + 25.0) continue;
    if (time < trap_cd_[t.id]) continue;
    if (crossed_gate(x0, z0, x1, z1, t.gate)) {
      trap_cd_[t.id] = time + 8.0;
      if (mph >= t.min_speed_mph) {
        const double prev = best_trap_mph(t.id);
        const bool pb = mph > prev + 0.05;
        if (pb) best_traps_[t.id] = mph;
        banner_is_zone_ = false;
        banner_title_ = pretty_speed_name(t.id, false);
        banner_speed_ = mph;
        banner_best_ = std::max(prev, mph);
        banner_new_pb_ = pb;
        banner_at_ = time;
        flash_at_ = time;
        out.push_back({SpeedActivityEvent::kTrapTriggered, t.id, banner_title_, mph, prev, pb, true});
      }
    }
  }

  // 2. Active Speed Zone progress / completion / cancellation.
  if (active_zone_idx_ >= 0 && size_t(active_zone_idx_) < data_.zones.size()) {
    const auto& z = data_.zones[size_t(active_zone_idx_)];
    zone_elapsed_ += dt;
    zone_dist_ += moved;
    if (mph < 10.0) zone_slow_t_ += dt;
    else zone_slow_t_ = 0.0;

    const SpeedGate& exit_gate = entry_gate_ == 1 ? z.gate2 : z.gate1;
    const double d_mid = std::hypot(x1 - z.midpoint[0], z1 - z.midpoint[2]);
    const double span = std::hypot(z.gate2.center[0] - z.gate1.center[0], z.gate2.center[2] - z.gate1.center[2]);

    if (zone_elapsed_ > 0.5 && crossed_gate(x0, z0, x1, z1, exit_gate)) {
      const double avg_mph = (zone_dist_ / std::max(0.1, zone_elapsed_)) * kMsToMph;
      const double prev = best_zone_mph(z.id);
      const bool pb = avg_mph >= z.min_speed_mph && avg_mph > prev + 0.05;
      if (pb) best_zones_[z.id] = avg_mph;
      banner_is_zone_ = true;
      banner_title_ = pretty_speed_name(z.id, true);
      banner_speed_ = avg_mph;
      banner_best_ = std::max(prev, avg_mph);
      banner_new_pb_ = pb;
      banner_at_ = time;
      flash_at_ = time;
      zone_cd_[z.id] = time + 8.0;
      out.push_back({SpeedActivityEvent::kZoneCompleted, z.id, banner_title_, avg_mph, prev, pb, true});
      active_zone_idx_ = -1;
      entry_gate_ = 0;
    } else if (zone_slow_t_ > 4.0 || zone_elapsed_ > 90.0 || d_mid > std::max(800.0, span * 1.5)) {
      out.push_back({SpeedActivityEvent::kZoneCancelled, z.id, pretty_speed_name(z.id, true), 0.0, 0.0, false, false});
      active_zone_idx_ = -1;
      entry_gate_ = 0;
    }
  } else {
    // Check Speed Zone entry (either gate 1 or gate 2).
    for (size_t i = 0; i < data_.zones.size(); ++i) {
      const auto& z = data_.zones[i];
      if (time < zone_cd_[z.id]) continue;
      int hit_gate = 0;
      if (std::hypot(x1 - z.gate1.center[0], z1 - z.gate1.center[2]) <= z.gate1.width + 25.0 &&
          crossed_gate(x0, z0, x1, z1, z.gate1))
        hit_gate = 1;
      else if (std::hypot(x1 - z.gate2.center[0], z1 - z.gate2.center[2]) <= z.gate2.width + 25.0 &&
               crossed_gate(x0, z0, x1, z1, z.gate2))
        hit_gate = 2;
      if (hit_gate > 0 && mph >= 15.0) {
        active_zone_idx_ = int(i);
        entry_gate_ = hit_gate;
        zone_elapsed_ = 0.0;
        zone_dist_ = 0.0;
        zone_slow_t_ = 0.0;
        flash_at_ = time;
        out.push_back({SpeedActivityEvent::kZoneEntered, z.id, pretty_speed_name(z.id, true), mph,
                       best_zone_mph(z.id), false, true});
        break;
      }
    }
  }

  return out;
}

SpeedActivityHud SpeedActivityTracker::hud(double time) const {
  SpeedActivityHud h;
  if (active_zone_idx_ >= 0 && size_t(active_zone_idx_) < data_.zones.size()) {
    h.zone_active = true;
    h.active_zone_id = pretty_speed_name(data_.zones[size_t(active_zone_idx_)].id, true);
    h.zone_elapsed_s = zone_elapsed_;
    h.zone_distance_m = zone_dist_;
    h.live_avg_mph = zone_elapsed_ > 0.05 ? (zone_dist_ / zone_elapsed_) * kMsToMph : 0.0;
  }
  const double since_banner = time - banner_at_;
  if (since_banner >= 0.0 && since_banner < 5.0 && !banner_title_.empty()) {
    h.banner_active = true;
    h.banner_is_zone = banner_is_zone_;
    h.banner_title = banner_title_;
    h.banner_speed_mph = banner_speed_;
    h.banner_best_mph = banner_best_;
    h.banner_new_pb = banner_new_pb_;
    h.banner_at = banner_at_;
  }
  const double since_flash = time - flash_at_;
  if (since_flash >= 0.0 && since_flash < 0.45) {
    h.flash_alpha = float(std::pow(1.0 - since_flash / 0.45, 2.0) * 0.78);
  }
  return h;
}

}  // namespace freeroam
