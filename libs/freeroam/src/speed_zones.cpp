#include "freeroam/speed_zones.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <sstream>

#include "audio/xml.h"
#include "fh1data/archive.h"
#include "fh1data/stringtables.h"

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

std::string resolve_str(const fh1::StringTable* tbl, std::string_view key) {
  if (!tbl || key.empty()) return {};
  const auto* u16 = tbl->find(fh1::string_hash(key));
  if (!u16) return {};
  return fh1::utf16_to_utf8(*u16);
}

std::string pretty_speed_name(const std::string& id, bool is_zone) {
  // Fallback format when List_SpeedCameras.str is absent:
  // "speed_camera_01" -> "SPEED TRAP 01", "average_speed_05" -> "SPEED ZONE 05".
  std::string num;
  for (size_t i = id.size(); i-- > 0;) {
    if (id[i] >= '0' && id[i] <= '9') num.insert(num.begin(), id[i]);
    else if (!num.empty()) break;
  }
  return (is_zone ? "SPEED ZONE " : "SPEED TRAP ") + (num.empty() ? id : num);
}

std::string resolve_speed_camera_name(const audio::XmlNode& act, const std::string& id, int scoreboard_id, bool is_zone,
                                      const fh1::StringTable* camera_strings) {
  if (camera_strings) {
    auto try_key = [&](std::string_view key) -> std::string {
      return key.empty() ? std::string{} : resolve_str(camera_strings, key);
    };
    for (const char* attr : {"nameDesc", "prompt", "mapDescription", "description", "displayName", "name_id"}) {
      if (std::string s = try_key(act.attr(attr)); !s.empty()) return s;
    }
    for (const auto& ch : act.children) {
      for (const char* attr : {"nameDesc", "prompt", "mapDescription", "description", "displayName"}) {
        if (std::string s = try_key(ch.attr(attr)); !s.empty()) return s;
      }
    }
    if (scoreboard_id > 0) {
      const std::string sid = std::to_string(scoreboard_id);
      if (std::string s = try_key("IDS_SpeedCamera_" + sid); !s.empty()) return s;
      if (is_zone) {
        if (std::string s = try_key("IDS_AverageSpeed_" + sid); !s.empty()) return s;
        if (std::string s = try_key("IDS_SpeedZone_" + sid); !s.empty()) return s;
      } else {
        if (std::string s = try_key("IDS_SpeedTrap_" + sid); !s.empty()) return s;
      }
    }
    if (std::string s = try_key(id); !s.empty()) return s;
  }
  return pretty_speed_name(id, is_zone);
}

std::string display_label_for(const SpeedTrapDef& t) {
  return !t.display_name.empty() ? t.display_name : pretty_speed_name(t.id, false);
}

std::string display_label_for(const SpeedZoneDef& z) {
  return !z.display_name.empty() ? z.display_name : pretty_speed_name(z.id, true);
}

bool load_str_from_zip(fh1::Archive& zip, std::string_view entry, fh1::StringTable& out) {
  auto bytes = zip.read(entry);
  if (!bytes.ok()) return false;
  return fh1::parse_string_table(bytes.data, out, nullptr);
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
                            std::string_view gameobjs_xml, SpeedActivitiesData& out, std::string* error,
                            const fh1::StringTable* camera_strings) {
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
    t.display_name = resolve_speed_camera_name(act, id, t.scoreboard_id, false, camera_strings);
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
    z.display_name = resolve_speed_camera_name(act, id, z.scoreboard_id, true, camera_strings);
    if (const auto* thr = act.child("SpeedThreshold")) z.min_speed_mph = thr->number("minimumSpeed", 30.0);
    z.gate1 = make_gate(il1->second, ir1->second);
    z.gate2 = make_gate(il2->second, ir2->second);
    z.midpoint = {0.5 * (z.gate1.center[0] + z.gate2.center[0]), 0.5 * (z.gate1.center[1] + z.gate2.center[1]),
                  0.5 * (z.gate1.center[2] + z.gate2.center[2])};
    const double gate_span =
        std::hypot(z.gate2.center[0] - z.gate1.center[0], z.gate2.center[2] - z.gate1.center[2]);
    double authored_len = 0.0;
    if (act.has("length")) authored_len = act.number("length", 0.0);
    else if (act.has("distance")) authored_len = act.number("distance", 0.0);
    else if (const auto* r = act.child("Route")) authored_len = r->number("length", r->number("distance", 0.0));
    z.corridor_distance_m = (authored_len > 1.0) ? authored_len : gate_span;
    out.zones.push_back(std::move(z));
  }

  return !out.traps.empty() || !out.zones.empty();
}

std::filesystem::path colorado_dirt_gamemodes(const std::filesystem::path& package_dir) {
  if (package_dir.empty()) return {};
  std::error_code ec;
  const auto zips = package_dir / "Media" / "DLCZips";
  if (!std::filesystem::is_directory(zips, ec)) return {};
  for (const auto& z : std::filesystem::directory_iterator(zips, ec)) {
    const auto d = z.path() / "Media" / "gamemodes" / "ColoradoDirt";
    if (std::filesystem::is_regular_file(d / "speed_camera.xml", ec) &&
        std::filesystem::is_regular_file(d / "average_speed.xml", ec))
      return d;
  }
  return {};
}

bool load_speed_activities(const std::filesystem::path& game_root, SpeedActivitiesData& out, std::string* error,
                           const std::filesystem::path& gamemodes_dir) {
  std::string why;
  std::vector<uint8_t> cam_bytes, avg_bytes;
  if (!gamemodes_dir.empty()) {
    auto slurp = [](const std::filesystem::path& p, std::vector<uint8_t>& b) {
      std::ifstream in(p, std::ios::binary);
      if (!in) return false;
      b.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
      return true;
    };
    if (!slurp(gamemodes_dir / "speed_camera.xml", cam_bytes))
      return fail(error, "cannot open " + (gamemodes_dir / "speed_camera.xml").string());
    if (!slurp(gamemodes_dir / "average_speed.xml", avg_bytes))
      return fail(error, "cannot open " + (gamemodes_dir / "average_speed.xml").string());
  } else {
    auto zip = fh1::Archive::open(game_root / "media" / "gamemodes.zip", &why);
    if (!zip) return fail(error, why);
    auto cam = zip->read("Colorado/speed_camera.xml");
    if (!cam.ok()) return fail(error, "Colorado/speed_camera.xml: " + cam.error);
    auto avg = zip->read("Colorado/average_speed.xml");
    if (!avg.ok()) return fail(error, "Colorado/average_speed.xml: " + avg.error);
    cam_bytes = std::move(cam.data);
    avg_bytes = std::move(avg.data);
  }

  std::ifstream go(game_root / "media" / "tracks" / "colorado" / "Ribbon_00" / "GameObjs.xml", std::ios::binary);
  if (!go) return fail(error, "cannot open Ribbon_00/GameObjs.xml");
  std::stringstream ss;
  ss << go.rdbuf();
  const std::string objs_str = ss.str();

  // Optional localized speed camera names from EN.zip:List_SpeedCameras.str (matching outposts.cpp).
  fh1::StringTable cam_str;
  if (auto en = fh1::Archive::open(game_root / "media" / "stringtables" / "EN.zip", nullptr)) {
    load_str_from_zip(*en, "List_SpeedCameras.str", cam_str);
  }

  auto sv = [](const std::vector<uint8_t>& d) { return std::string_view(reinterpret_cast<const char*>(d.data()), d.size()); };
  return parse_speed_activities(sv(cam_bytes), sv(avg_bytes), objs_str, out, error,
                                cam_str.keys.empty() ? nullptr : &cam_str);
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

void SpeedActivityTracker::cancel_active() {
  active_zone_idx_ = -1;
  entry_gate_ = 0;
  zone_elapsed_ = 0.0;
  zone_dist_ = 0.0;
  zone_corridor_progress_ = 0.0;
  zone_slow_t_ = 0.0;
  zone_off_road_t_ = 0.0;
}

void SpeedActivityTracker::set_off_road(bool off_road) {
  off_road_ = off_road;
  if (!off_road_) zone_off_road_t_ = 0.0;
}

void SpeedActivityTracker::reset_session_clock() {
  trap_cd_.clear();
  zone_cd_.clear();
  cancel_active();
  off_road_ = false;
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
        banner_title_ = display_label_for(t);
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
    if (off_road_) zone_off_road_t_ += dt;
    else zone_off_road_t_ = 0.0;

    const SpeedGate& entry_gate = entry_gate_ == 1 ? z.gate1 : z.gate2;
    const SpeedGate& exit_gate = entry_gate_ == 1 ? z.gate2 : z.gate1;
    const double d_mid = std::hypot(x1 - z.midpoint[0], z1 - z.midpoint[2]);
    const double span = std::hypot(z.gate2.center[0] - z.gate1.center[0], z.gate2.center[2] - z.gate1.center[2]);
    const double corridor_len = z.corridor_distance_m > 1.0 ? z.corridor_distance_m : span;

    // Project progress along the entry->exit corridor axis so live distance cannot
    // be inflated by zig-zagging across the road.
    if (span > 1.0) {
      const double ax = (exit_gate.center[0] - entry_gate.center[0]) / span;
      const double az = (exit_gate.center[2] - entry_gate.center[2]) / span;
      const double proj = (x1 - entry_gate.center[0]) * ax + (z1 - entry_gate.center[2]) * az;
      const double frac = std::clamp(proj / span, 0.0, 1.0);
      zone_corridor_progress_ = std::min(zone_dist_, std::max(zone_corridor_progress_, frac * corridor_len));
    } else {
      zone_corridor_progress_ = std::min(zone_dist_, corridor_len);
    }

    if (zone_off_road_t_ > kOffRoadFailGraceS) {
      SpeedActivityEvent fail_ev{SpeedActivityEvent::kZoneCancelled, z.id, display_label_for(z), 0.0, 0.0, false, false};
      fail_ev.failed_off_road = true;
      out.push_back(std::move(fail_ev));
      zone_cd_[z.id] = time + 3.0;
      cancel_active();
    } else if (zone_elapsed_ > 0.5 && crossed_gate(x0, z0, x1, z1, exit_gate)) {
      // Score strictly against authored corridor length (clamped from gate geometry),
      // never raw zig-zag odometer distance.
      const double scored_dist = corridor_len > 1.0 ? corridor_len : zone_dist_;
      const double avg_mph = (scored_dist / std::max(0.1, zone_elapsed_)) * kMsToMph;
      const double prev = best_zone_mph(z.id);
      const bool pb = avg_mph >= z.min_speed_mph && avg_mph > prev + 0.05;
      if (pb) best_zones_[z.id] = avg_mph;
      banner_is_zone_ = true;
      banner_title_ = display_label_for(z);
      banner_speed_ = avg_mph;
      banner_best_ = std::max(prev, avg_mph);
      banner_new_pb_ = pb;
      banner_at_ = time;
      flash_at_ = time;
      zone_cd_[z.id] = time + 8.0;
      out.push_back({SpeedActivityEvent::kZoneCompleted, z.id, banner_title_, avg_mph, prev, pb, true});
      cancel_active();
    } else if (zone_slow_t_ > 4.0 || zone_elapsed_ > 90.0 || d_mid > std::max(800.0, span * 1.5)) {
      out.push_back({SpeedActivityEvent::kZoneCancelled, z.id, display_label_for(z), 0.0, 0.0, false, false});
      cancel_active();
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
        zone_corridor_progress_ = 0.0;
        zone_slow_t_ = 0.0;
        zone_off_road_t_ = 0.0;
        flash_at_ = time;
        out.push_back({SpeedActivityEvent::kZoneEntered, z.id, display_label_for(z), mph,
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
    h.active_zone_id = display_label_for(data_.zones[size_t(active_zone_idx_)]);
    h.zone_elapsed_s = zone_elapsed_;
    h.zone_distance_m = zone_corridor_progress_;
    h.live_avg_mph = zone_elapsed_ > 0.05 ? (zone_corridor_progress_ / zone_elapsed_) * kMsToMph : 0.0;
    h.off_road_warning = off_road_ && zone_off_road_t_ > 0.0;
    h.off_road_timer_s = zone_off_road_t_;
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
