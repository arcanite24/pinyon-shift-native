#include "race/showcase.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>

namespace race {
namespace {

constexpr double kPi = 3.14159265358979323846;

double wrap_angle(double a) {
  while (a > kPi) a -= 2.0 * kPi;
  while (a < -kPi) a += 2.0 * kPi;
  return a;
}

std::string lower_ascii(std::string_view sv) {
  std::string out(sv);
  for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool contains_ci(std::string_view hay, std::string_view needle) {
  return lower_ascii(hay).find(lower_ascii(needle)) != std::string::npos;
}

void apply_visual_defaults(ShowcaseSpec& s) {
  switch (s.kind) {
    case ShowcaseAircraftKind::kMustangP51:
      s.display_name = "P-51 MUSTANG";
      s.base_altitude_m = 26.0;
      s.swoop_altitude_m = 15.0;
      s.lateral_sway_m = 15.0;
      s.target_speed_mps = 52.0;
      s.scale = 1.45;
      break;
    case ShowcaseAircraftKind::kHelicopter:
      s.display_name = "HELICOPTER";
      s.base_altitude_m = 21.0;
      s.swoop_altitude_m = 12.5;
      s.lateral_sway_m = 12.0;
      s.target_speed_mps = 48.0;
      s.scale = 1.2;
      break;
    case ShowcaseAircraftKind::kBiplane:
      s.display_name = "BIPLANE";
      s.base_altitude_m = 24.0;
      s.swoop_altitude_m = 14.5;
      s.lateral_sway_m = 14.0;
      s.target_speed_mps = 46.0;
      s.scale = 1.15;
      break;
    case ShowcaseAircraftKind::kHotAirBalloon:
      s.display_name = "HOT-AIR BALLOON";
      s.base_altitude_m = 36.0;
      s.swoop_altitude_m = 22.0;
      s.lateral_sway_m = 10.0;
      s.target_speed_mps = 44.0;
      s.scale = 1.35;
      break;
    case ShowcaseAircraftKind::kStuntJet:
    default:
      s.display_name = "SHOWCASE AIRCRAFT";
      s.base_altitude_m = 26.0;
      s.swoop_altitude_m = 15.0;
      s.lateral_sway_m = 15.0;
      s.target_speed_mps = 50.0;
      s.scale = 1.15;
      break;
  }
}

void fill_uniform_difficulty(ShowcaseSpec& s, double base_m, double min_m, double max_m) {
  s.base_mult = base_m;
  s.rubber_band_min_mult = min_m;
  s.rubber_band_max_mult = max_m;
  for (auto& d : s.difficulty_speeds) {
    d.base_mult = base_m;
    d.rubber_band_min_mult = min_m;
    d.rubber_band_max_mult = max_m;
  }
}

std::string horizon_id_for_event(int64_t event_id) {
  switch (event_id) {
    case 101: return "PLANE_RACE_001";
    case 168: return "PLANE_RACE_002";
    case 172: return "PLANE_RACE_003";
    case 223: return "PLANE_RACE_004";
    case 220: return "PLANE_RACE_005";
    case 222: return "PLANE_RACE_006";
    case 246: return "PLANE_RACE_007";
    default: return {};
  }
}

ShowcaseAircraftKind infer_kind(int64_t event_id, std::string_view pgeo, std::string_view refname) {
  if (contains_ci(pgeo, "balloon") || contains_ci(refname, "balloon")) return ShowcaseAircraftKind::kHotAirBalloon;
  if (contains_ci(pgeo, "helicopter") || contains_ci(refname, "helicopter") || contains_ci(refname, "chopper"))
    return ShowcaseAircraftKind::kHelicopter;
  if (contains_ci(pgeo, "biplane") || contains_ci(refname, "biplane")) return ShowcaseAircraftKind::kBiplane;
  if (contains_ci(pgeo, "mustang") || contains_ci(pgeo, "p51") || contains_ci(refname, "mustang") ||
      contains_ci(refname, "p51"))
    return ShowcaseAircraftKind::kMustangP51;
  switch (event_id) {
    case 101:
    case 222: return ShowcaseAircraftKind::kMustangP51;
    case 168:
    case 220: return ShowcaseAircraftKind::kHelicopter;
    case 172:
    case 246: return ShowcaseAircraftKind::kBiplane;
    case 223: return ShowcaseAircraftKind::kHotAirBalloon;
    default: return ShowcaseAircraftKind::kMustangP51;
  }
}

// Strips XML comments (`<!-- ... -->`) so commented-out tags never confuse parsing.
std::string strip_xml_comments(std::string_view xml) {
  std::string out;
  out.reserve(xml.size());
  size_t i = 0;
  while (i < xml.size()) {
    if (i + 3 < xml.size() && xml[i] == '<' && xml[i + 1] == '!' && xml[i + 2] == '-' && xml[i + 3] == '-') {
      const size_t end = xml.find("-->", i + 4);
      if (end == std::string_view::npos) break;
      i = end + 3;
    } else {
      out.push_back(xml[i++]);
    }
  }
  return out;
}

struct XmlTag {
  std::string name;
  bool closing = false;
  std::map<std::string, std::string> attrs;

  std::string attr(std::string_view key) const {
    auto it = attrs.find(lower_ascii(key));
    return it == attrs.end() ? std::string() : it->second;
  }
  bool has(std::string_view key) const { return attrs.find(lower_ascii(key)) != attrs.end(); }
  double number(std::string_view key, double def = 0.0) const {
    auto it = attrs.find(lower_ascii(key));
    if (it == attrs.end() || it->second.empty()) return def;
    char* end = nullptr;
    const double v = std::strtod(it->second.c_str(), &end);
    return (end && end != it->second.c_str() && std::isfinite(v)) ? v : def;
  }
  bool boolean(std::string_view key, bool def = false) const {
    auto it = attrs.find(lower_ascii(key));
    if (it == attrs.end()) return def;
    const std::string v = lower_ascii(it->second);
    if (v == "true" || v == "1") return true;
    if (v == "false" || v == "0") return false;
    return def;
  }
};

// Tolerant XML tag scanner: handles malformed closing tags like `</DifficulySettings\n<DifficulySettings>`
// by terminating a tag scan when another `<` appears outside quotes.
std::vector<XmlTag> scan_xml_tags(std::string_view text) {
  std::vector<XmlTag> tags;
  const size_t n = text.size();
  size_t i = 0;
  while (i < n) {
    const size_t lt = text.find('<', i);
    if (lt == std::string_view::npos) break;
    i = lt + 1;
    if (i >= n) break;
    if (text[i] == '?' || text[i] == '!') {
      const size_t gt = text.find('>', i);
      if (gt == std::string_view::npos) break;
      i = gt + 1;
      continue;
    }
    XmlTag tag;
    if (text[i] == '/') {
      tag.closing = true;
      ++i;
    }
    while (i < n && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    const size_t name_start = i;
    while (i < n && !std::isspace(static_cast<unsigned char>(text[i])) && text[i] != '>' && text[i] != '/' &&
           text[i] != '<') {
      ++i;
    }
    tag.name = lower_ascii(text.substr(name_start, i - name_start));
    // Parse attributes until `>` or an unclosed next `<`.
    while (i < n) {
      while (i < n && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
      if (i >= n) break;
      if (text[i] == '>') {
        ++i;
        break;
      }
      if (text[i] == '<') {
        // Missing `>` on a closing tag; leave `i` at `<` for the next tag.
        break;
      }
      if (text[i] == '/') {
        ++i;
        continue;
      }
      const size_t ks = i;
      while (i < n && !std::isspace(static_cast<unsigned char>(text[i])) && text[i] != '=' && text[i] != '>' &&
             text[i] != '<') {
        ++i;
      }
      const std::string key = lower_ascii(text.substr(ks, i - ks));
      while (i < n && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
      if (i < n && text[i] == '=') {
        ++i;
        while (i < n && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        std::string val;
        if (i < n && (text[i] == '"' || text[i] == '\'')) {
          const char q = text[i++];
          const size_t vs = i;
          while (i < n && text[i] != q) ++i;
          val = std::string(text.substr(vs, i - vs));
          if (i < n && text[i] == q) ++i;
        } else {
          const size_t vs = i;
          while (i < n && !std::isspace(static_cast<unsigned char>(text[i])) && text[i] != '>' && text[i] != '<') {
            ++i;
          }
          val = std::string(text.substr(vs, i - vs));
        }
        if (!key.empty()) tag.attrs[key] = std::move(val);
      }
    }
    if (!tag.name.empty()) tags.push_back(std::move(tag));
  }
  return tags;
}

}  // namespace

bool is_airborne_showcase(int career_type, std::string_view horizon_id) {
  if (career_type == 8) return true;
  return horizon_id.rfind("PLANE_RACE_", 0) == 0;
}

ShowcaseSpec showcase_spec_for(std::string_view horizon_id) {
  ShowcaseSpec s;
  s.horizon_id = std::string(horizon_id);
  if (horizon_id == "PLANE_RACE_001") {
    s.event_id = 101;
    s.kind = ShowcaseAircraftKind::kMustangP51;
    apply_visual_defaults(s);
    s.anim_pgeo = "ANIM_Timed_Event_Test_Animation.pgeo";
    s.anim_in_race = "ANIM_Timed_Event_Test_Animation";
    s.on_countdown = false;
    s.cross_start_time_s = 9.44;
    s.cross_finish_time_s = 171.68;
    s.rubber_band_spring = 30000.0;
    s.rubber_band_accel_rate = 0.4;
    s.rubber_band_decel_rate = 0.15;
    s.rubber_band_offset_ahead = -0.001;
    fill_uniform_difficulty(s, 1.0, 0.75, 1.3);
    s.rumble_strength = 0.8;
    s.rumble_distance_outer_m = 80.0;
    s.rumble_distance_inner_m = 15.0;
  } else if (horizon_id == "PLANE_RACE_002") {
    s.event_id = 168;
    s.kind = ShowcaseAircraftKind::kHelicopter;
    apply_visual_defaults(s);
    s.anim_pgeo = "ANIM_Showcase_Event_Helicopter_Race.pgeo";
    s.anim_in_race = "ANIM_Showcase_Event_Helicopter_Race";
    s.on_countdown = false;
    s.cross_start_time_s = 0.0;
    s.cross_finish_time_s = 121.94;
    s.rubber_band_spring = 30000.0;
    s.rubber_band_accel_rate = 0.1;
    s.rubber_band_decel_rate = 0.1;
    s.rubber_band_offset_ahead = 0.001;
    fill_uniform_difficulty(s, 1.0, 0.9, 1.1);
    s.rumble_strength = 0.6;
    s.rumble_distance_outer_m = 10.0;
    s.rumble_distance_inner_m = 1.0;
  } else if (horizon_id == "PLANE_RACE_003") {
    s.event_id = 172;
    s.kind = ShowcaseAircraftKind::kBiplane;
    apply_visual_defaults(s);
    s.anim_pgeo = "ANIM_Showcase_Event_Biplane_Race.pgeo";
    s.anim_in_race = "ANIM_Showcase_Event_Biplane_Race";
    s.on_countdown = false;
    s.cross_start_time_s = 5.9;
    s.cross_finish_time_s = 134.70;
    s.rubber_band_spring = 30000.0;
    s.rubber_band_accel_rate = 0.2;
    s.rubber_band_decel_rate = 0.1;
    s.rubber_band_offset_ahead = 0.0;
    fill_uniform_difficulty(s, 1.0, 0.8, 1.2);
    s.rumble_strength = 0.8;
    s.rumble_distance_outer_m = 80.0;
    s.rumble_distance_inner_m = 15.0;
  } else if (horizon_id == "PLANE_RACE_004") {
    s.event_id = 223;
    s.kind = ShowcaseAircraftKind::kHotAirBalloon;
    apply_visual_defaults(s);
    s.anim_pgeo = "ANIM_Showcase_Event_Balloon_Race.pgeo";
    s.anim_in_race = "ANIM_Showcase_Event_Balloon_Race";
    s.on_countdown = true;
    s.cross_start_time_s = 0.20;
    s.cross_finish_time_s = 96.69;
    s.rubber_band_spring = 300.0;
    s.rubber_band_accel_rate = 0.1;
    s.rubber_band_decel_rate = 0.04;
    s.rubber_band_offset_ahead = -0.006;
    fill_uniform_difficulty(s, 1.0, 0.7, 1.3);
    s.rumble_strength = 0.0;
    s.rumble_distance_outer_m = 8.0;
    s.rumble_distance_inner_m = 1.0;
  } else if (horizon_id == "PLANE_RACE_005") {
    s.event_id = 220;
    s.kind = ShowcaseAircraftKind::kHelicopter;
    apply_visual_defaults(s);
    s.anim_pgeo = "ANIM_Showcase_Event_Helicopter_Race_2.pgeo";
    s.anim_in_race = "ANIM_Showcase_Event_Helicopter_Race_2";
    s.on_countdown = false;
    s.cross_start_time_s = 3.50;
    s.cross_finish_time_s = 121.83;
    s.rubber_band_spring = 30000.0;
    s.rubber_band_accel_rate = 0.1;
    s.rubber_band_decel_rate = 0.1;
    s.rubber_band_offset_ahead = 0.001;
    fill_uniform_difficulty(s, 1.0, 0.9, 1.1);
    s.rumble_strength = 0.2;
    s.rumble_distance_outer_m = 30.0;
    s.rumble_distance_inner_m = 5.0;
  } else if (horizon_id == "PLANE_RACE_006") {
    s.event_id = 222;
    s.kind = ShowcaseAircraftKind::kMustangP51;
    apply_visual_defaults(s);
    s.anim_pgeo = "ANIM_Showcase_Event_P51_Mustang_2.pgeo";
    s.anim_in_race = "ANIM_Showcase_Event_P51_Mustang_2";
    s.on_countdown = false;
    s.cross_start_time_s = 2.67;
    s.cross_finish_time_s = 178.03;
    s.rubber_band_spring = 30000.0;
    s.rubber_band_accel_rate = 0.4;
    s.rubber_band_decel_rate = 0.15;
    s.rubber_band_offset_ahead = -0.0001;
    fill_uniform_difficulty(s, 1.0, 0.75, 1.3);
    s.rumble_strength = 0.8;
    s.rumble_distance_outer_m = 80.0;
    s.rumble_distance_inner_m = 15.0;
  } else if (horizon_id == "PLANE_RACE_007") {
    s.event_id = 246;
    s.kind = ShowcaseAircraftKind::kBiplane;
    apply_visual_defaults(s);
    s.anim_pgeo = "ANIM_Showcase_Event_Biplane_Race_2.pgeo";
    s.anim_in_race = "ANIM_Showcase_Event_Biplane_Race_2";
    s.on_countdown = false;
    s.cross_start_time_s = 2.90;
    s.cross_finish_time_s = 108.90;
    s.rubber_band_spring = 30000.0;
    s.rubber_band_accel_rate = 0.4;
    s.rubber_band_decel_rate = 0.15;
    s.rubber_band_offset_ahead = -0.0001;
    fill_uniform_difficulty(s, 1.0, 0.8, 1.2);
    s.rumble_strength = 0.8;
    s.rumble_distance_outer_m = 80.0;
    s.rumble_distance_inner_m = 15.0;
  } else {
    s.kind = ShowcaseAircraftKind::kMustangP51;
    apply_visual_defaults(s);
    s.display_name = "SHOWCASE AIRCRAFT";
    fill_uniform_difficulty(s, 1.0, 0.75, 1.3);
  }
  return s;
}

ShowcaseSpec showcase_spec_for_event(int64_t event_id, std::string_view horizon_id) {
  const std::string mapped = horizon_id_for_event(event_id);
  if (!mapped.empty()) return showcase_spec_for(mapped);
  ShowcaseSpec s = showcase_spec_for(horizon_id);
  if (event_id > 0) s.event_id = event_id;
  return s;
}

bool parse_airborne_challenges_xml(std::string_view xml, std::vector<ShowcaseSpec>& out, std::string* error) {
  out.clear();
  const std::string clean = strip_xml_comments(xml);
  const auto tags = scan_xml_tags(clean);
  if (tags.empty()) {
    if (error) *error = "airborne_challenges.xml: empty or invalid XML";
    return false;
  }

  bool in_event = false;
  ShowcaseSpec cur;
  std::string refname;
  int diff_count = 0;

  auto flush = [&]() {
    if (!in_event) return;
    if (cur.event_id > 0) {
      cur.kind = infer_kind(cur.event_id, cur.anim_pgeo, refname);
      const ShowcaseSpec def = showcase_spec_for_event(cur.event_id, cur.horizon_id);
      if (cur.horizon_id.empty()) cur.horizon_id = def.horizon_id;
      cur.display_name = def.display_name;
      cur.base_altitude_m = def.base_altitude_m;
      cur.swoop_altitude_m = def.swoop_altitude_m;
      cur.lateral_sway_m = def.lateral_sway_m;
      cur.target_speed_mps = def.target_speed_mps;
      cur.scale = def.scale;
      if (diff_count > 0) {
        // Fill any missing difficulty slots from the last parsed slot.
        for (int i = diff_count; i < 4; ++i) cur.difficulty_speeds[size_t(i)] = cur.difficulty_speeds[size_t(diff_count - 1)];
        cur.base_mult = cur.difficulty_speeds[0].base_mult;
        cur.rubber_band_min_mult = cur.difficulty_speeds[0].rubber_band_min_mult;
        cur.rubber_band_max_mult = cur.difficulty_speeds[0].rubber_band_max_mult;
      }
      out.push_back(std::move(cur));
    }
    in_event = false;
    cur = ShowcaseSpec{};
    refname.clear();
    diff_count = 0;
  };

  for (const auto& t : tags) {
    if (t.name == "challengeevent") {
      if (t.closing) {
        flush();
      } else {
        flush();
        in_event = true;
        cur = ShowcaseSpec{};
        cur.event_id = int64_t(std::llround(t.number("eventid", 0.0)));
        cur.horizon_id = horizon_id_for_event(cur.event_id);
        refname = t.attr("refname");
        fill_uniform_difficulty(cur, 1.0, 0.75, 1.3);
      }
      continue;
    }
    if (!in_event || t.closing) continue;

    if (t.name == "names") {
      if (t.has("objectfilename")) cur.anim_pgeo = t.attr("objectfilename");
      if (t.has("anim_in_race")) cur.anim_in_race = t.attr("anim_in_race");
    } else if (t.name == "opponent") {
      if (t.has("locstringpilotname")) cur.pilot_name_ids = t.attr("locstringpilotname");
    } else if (t.name == "animstart") {
      cur.on_countdown = t.boolean("on_countdown", false);
    } else if (t.name == "timing") {
      cur.cross_start_time_s = t.number("in_race_anim_cross_start_time", cur.cross_start_time_s);
      cur.cross_finish_time_s = t.number("in_race_anim_cross_finish_time", cur.cross_finish_time_s);
    } else if (t.name == "checkpoint") {
      ShowcaseTimingCheckpoint cp;
      cp.time_s = t.number("time", 0.0);
      cp.spline_dist_to_next = t.number("splinedisttonext", 0.0);
      cp.additional_rb_offset = t.number("additionalrubberbandoffset", 0.0);
      cur.timing_checkpoints.push_back(cp);
    } else if (t.name == "rubberbanding") {
      cur.rubber_band_spring = t.number("rubber_band_spring", cur.rubber_band_spring);
      cur.rubber_band_accel_rate = t.number("accel_rate", cur.rubber_band_accel_rate);
      cur.rubber_band_decel_rate = t.number("decel_rate", cur.rubber_band_decel_rate);
      cur.rubber_band_offset_ahead = t.number("rubber_band_offset_ahead", cur.rubber_band_offset_ahead);
      cur.rubber_band_checkpoint_adjustment =
          t.number("rubber_band_checkpoint_adjustment", cur.rubber_band_checkpoint_adjustment);
    } else if (t.name == "animspeeds") {
      if (diff_count < 4) {
        auto& ds = cur.difficulty_speeds[size_t(diff_count++)];
        ds.base_mult = t.number("base_mult", 1.0);
        ds.rubber_band_min_mult = t.number("rubber_band_min_mult", 0.75);
        ds.rubber_band_max_mult = t.number("rubber_band_max_mult", 1.3);
      }
    } else if (t.name == "rumblewhennearplayer") {
      cur.rumble_strength = t.number("strength", cur.rumble_strength);
      cur.rumble_distance_outer_m = t.number("distance_outer", cur.rumble_distance_outer_m);
      cur.rumble_distance_inner_m = t.number("distance_inner", cur.rumble_distance_inner_m);
    }
  }
  flush();

  if (out.empty()) {
    if (error) *error = "airborne_challenges.xml: no ChallengeEvent entries found";
    return false;
  }
  return true;
}

bool load_airborne_challenges(const std::filesystem::path& game_root, std::vector<ShowcaseSpec>& out,
                              std::string* error) {
  const auto path = game_root / "media" / "tracks" / "colorado" / "airborne_challenges.xml";
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (error) *error = "cannot open " + path.string();
    return false;
  }
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return parse_airborne_challenges_xml(text, out, error);
}

ShowcaseSpec resolve_showcase_spec(int64_t event_id, std::string_view horizon_id,
                                   const std::vector<ShowcaseSpec>& xml_specs, int difficulty) {
  ShowcaseSpec spec = showcase_spec_for_event(event_id, horizon_id);
  for (const auto& xs : xml_specs) {
    const bool match_id = (event_id > 0 && xs.event_id == event_id);
    const bool match_hid = (!horizon_id.empty() && xs.horizon_id == horizon_id);
    if (match_id || match_hid) {
      spec = xs;
      if (spec.horizon_id.empty() && !horizon_id.empty()) spec.horizon_id = std::string(horizon_id);
      break;
    }
  }
  const int tier = std::clamp(difficulty, 0, 3);
  const auto& ds = spec.difficulty_speeds[size_t(tier)];
  if (ds.base_mult > 0.0 && ds.rubber_band_max_mult >= ds.rubber_band_min_mult && ds.rubber_band_max_mult > 0.0) {
    spec.base_mult = ds.base_mult;
    spec.rubber_band_min_mult = ds.rubber_band_min_mult;
    spec.rubber_band_max_mult = ds.rubber_band_max_mult;
  }
  return spec;
}

void ShowcaseOpponent::build_checkpoint_table() {
  cp_times_.clear();
  cp_fracs_.clear();
  cp_rb_offsets_.clear();
  use_xml_checkpoints_ = false;

  // Retail TimingCheckpoints layout:
  //   [0]: t = 0.0 (anim start -> start line)
  //   [1]: t = cross_start_time_s (start line -> cp 1)
  //   ...
  //   [last]: t = cross_finish_time_s (finish line -> anim end)
  const auto& cps = spec_.timing_checkpoints;
  if (cps.size() < 3) return;

  // Start at index 1 (start line) through the final checkpoint (finish line).
  const size_t start_idx = 1;
  const size_t end_idx = cps.size() - 1;
  if (end_idx <= start_idx) return;

  double total_spline_dist = 0.0;
  for (size_t i = start_idx; i < end_idx; ++i) {
    total_spline_dist += std::max(0.0, cps[i].spline_dist_to_next);
  }
  const double total_time = std::max(1e-3, cps[end_idx].time_s - cps[start_idx].time_s);
  const bool use_spline_weights = total_spline_dist > 10.0;

  double cum_dist = 0.0;
  for (size_t i = start_idx; i <= end_idx; ++i) {
    cp_times_.push_back(cps[i].time_s);
    const double frac = use_spline_weights
                            ? std::clamp(cum_dist / total_spline_dist, 0.0, 1.0)
                            : std::clamp((cps[i].time_s - cps[start_idx].time_s) / total_time, 0.0, 1.0);
    cp_fracs_.push_back(frac);
    cp_rb_offsets_.push_back(cps[i].additional_rb_offset);
    if (i < end_idx) cum_dist += std::max(0.0, cps[i].spline_dist_to_next);
  }
  if (cp_times_.size() >= 2 && cp_times_.back() > cp_times_.front()) {
    cp_fracs_.front() = 0.0;
    cp_fracs_.back() = 1.0;
    use_xml_checkpoints_ = true;
  }
}

double ShowcaseOpponent::frac_from_anim_time(double t, double* extra_rb_offset) const {
  if (extra_rb_offset) *extra_rb_offset = 0.0;
  if (use_xml_checkpoints_ && cp_times_.size() >= 2) {
    if (t <= cp_times_.front()) {
      if (extra_rb_offset) *extra_rb_offset = cp_rb_offsets_.front();
      return 0.0;
    }
    if (t >= cp_times_.back()) {
      if (extra_rb_offset) *extra_rb_offset = cp_rb_offsets_.back();
      return 1.0;
    }
    auto it = std::upper_bound(cp_times_.begin(), cp_times_.end(), t);
    const size_t hi = std::clamp<size_t>(size_t(it - cp_times_.begin()), 1, cp_times_.size() - 1);
    const size_t lo = hi - 1;
    const double span = std::max(1e-6, cp_times_[hi] - cp_times_[lo]);
    const double u = std::clamp((t - cp_times_[lo]) / span, 0.0, 1.0);
    if (extra_rb_offset) *extra_rb_offset = cp_rb_offsets_[lo];
    return cp_fracs_[lo] + u * (cp_fracs_[hi] - cp_fracs_[lo]);
  }
  const double dur = std::max(1e-3, nominal_duration_s_);
  return std::clamp((t - spec_.cross_start_time_s) / dur, 0.0, 1.0);
}

std::array<double, 3> showcase_local_dir(const ShowcasePose& pose, double lx, double ly, double lz) {
  const double cy = std::cos(pose.heading), sy = std::sin(pose.heading);
  const double cp = std::cos(pose.pitch), sp = std::sin(pose.pitch);
  const double cr = std::cos(pose.roll), sr = std::sin(pose.roll);
  // Local (+X right, +Y up, +Z forward) -> game world rotation (roll, pitch, yaw).
  const double x1 = lx * cr - ly * sr;
  const double y1 = lx * sr + ly * cr;
  const double z1 = lz;
  const double x2 = x1;
  const double y2 = y1 * cp + z1 * sp;
  const double z2 = -y1 * sp + z1 * cp;
  const double wx = x2 * cy + z2 * sy;
  const double wy = y2;
  const double wz = -x2 * sy + z2 * cy;
  return {wx, wy, wz};
}

std::array<double, 3> showcase_local_point(const ShowcaseSpec& spec, const ShowcasePose& pose,
                                           double lx, double ly, double lz) {
  const double sc = spec.scale > 0.0 ? spec.scale : 1.0;
  const auto d = showcase_local_dir(pose, lx * sc, ly * sc, lz * sc);
  return {pose.position[0] + d[0], pose.position[1] + d[1], pose.position[2] + d[2]};
}

ShowcaseVfxAnchors compute_showcase_vfx_anchors(const ShowcaseSpec& spec, const ShowcasePose& pose) {
  ShowcaseVfxAnchors a;
  a.forward = showcase_local_dir(pose, 0.0, 0.0, 1.0);
  a.right = showcase_local_dir(pose, 1.0, 0.0, 0.0);
  a.up = showcase_local_dir(pose, 0.0, 1.0, 0.0);
  a.velocity = {a.forward[0] * pose.speed_mps, a.forward[1] * pose.speed_mps, a.forward[2] * pose.speed_mps};
  a.ground_wash_pos = {pose.position[0], pose.ground_y, pose.position[2]};
  a.agl_m = std::max(0.0, pose.position[1] - pose.ground_y);

  switch (spec.kind) {
    case ShowcaseAircraftKind::kMustangP51:
      // Twin Rolls-Royce Merlin V12 exhaust banks along the forward nose flanks.
      a.exhaust_pos[0] = showcase_local_point(spec, pose, -0.68, 0.14, 3.05);
      a.exhaust_pos[1] = showcase_local_point(spec, pose, 0.68, 0.14, 3.05);
      a.exhaust_count = 2;
      // Swept laminar wingtip trailing-edge vortex anchors (matches build_showcase_mesh span = 6.7).
      a.wingtip_pos[0] = showcase_local_point(spec, pose, -6.70, 0.18, -0.15);
      a.wingtip_pos[1] = showcase_local_point(spec, pose, 6.70, 0.18, -0.15);
      a.wingtip_count = 2;
      a.prop_hub_pos = showcase_local_point(spec, pose, 0.0, 0.0, 5.55);
      break;
    case ShowcaseAircraftKind::kBiplane:
      a.exhaust_pos[0] = showcase_local_point(spec, pose, -0.66, 0.08, 2.85);
      a.exhaust_pos[1] = showcase_local_point(spec, pose, 0.66, 0.08, 2.85);
      a.exhaust_count = 2;
      a.wingtip_pos[0] = showcase_local_point(spec, pose, -6.40, 1.48, 0.15);
      a.wingtip_pos[1] = showcase_local_point(spec, pose, 6.40, 1.48, 0.15);
      a.wingtip_count = 2;
      a.prop_hub_pos = showcase_local_point(spec, pose, 0.0, 0.0, 5.55);
      break;
    case ShowcaseAircraftKind::kStuntJet:
      a.exhaust_pos[0] = showcase_local_point(spec, pose, 0.0, 0.0, -5.45);
      a.exhaust_count = 1;
      a.wingtip_pos[0] = showcase_local_point(spec, pose, -5.90, -0.18, -3.20);
      a.wingtip_pos[1] = showcase_local_point(spec, pose, 5.90, -0.18, -3.20);
      a.wingtip_count = 2;
      a.prop_hub_pos = showcase_local_point(spec, pose, 0.0, 0.0, 5.55);
      break;
    case ShowcaseAircraftKind::kHelicopter:
      a.exhaust_pos[0] = showcase_local_point(spec, pose, -0.62, 0.55, -1.10);
      a.exhaust_pos[1] = showcase_local_point(spec, pose, 0.62, 0.55, -1.10);
      a.exhaust_count = 2;
      a.wingtip_count = 0;
      a.prop_hub_pos = showcase_local_point(spec, pose, 0.0, 1.55, 0.35);
      break;
    case ShowcaseAircraftKind::kHotAirBalloon:
      a.exhaust_pos[0] = showcase_local_point(spec, pose, 0.0, 2.55, 0.0);
      a.exhaust_count = 1;
      a.wingtip_count = 0;
      a.prop_hub_pos = showcase_local_point(spec, pose, 0.0, 2.35, 0.0);
      break;
  }
  return a;
}

double ShowcaseOpponent::min_clearance_m() const {
  switch (spec_.kind) {
    case ShowcaseAircraftKind::kHotAirBalloon: return 22.0;
    case ShowcaseAircraftKind::kHelicopter:    return 12.0;
    case ShowcaseAircraftKind::kMustangP51:
    case ShowcaseAircraftKind::kBiplane:
    case ShowcaseAircraftKind::kStuntJet:
    default:                                   return 14.0;
  }
}

void ShowcaseOpponent::init(const ShowcaseSpec& spec, const drive::RacingLine& line, double start_progress,
                            double finish_progress, double expected_car_speed_mps) {
  spec_ = spec;
  line_ = &line;
  start_p_ = start_progress;
  finish_p_ = std::max(start_progress + 200.0, finish_progress);
  const double run_len = finish_p_ - start_p_;
  target_speed_ = expected_car_speed_mps > 15.0 ? 0.96 * expected_car_speed_mps : spec_.target_speed_mps;
  build_checkpoint_table();
  const double xml_dur = spec_.cross_finish_time_s - spec_.cross_start_time_s;
  if (use_xml_checkpoints_ && xml_dur > 1.0) {
    nominal_duration_s_ = xml_dur;
  } else if (xml_dur > 1.0 && run_len > 2500.0 && expected_car_speed_mps <= 15.0) {
    nominal_duration_s_ = xml_dur;
  } else {
    nominal_duration_s_ = std::max(1.0, run_len / std::max(10.0, target_speed_));
  }
  anim_time_ = spec_.on_countdown ? 0.0 : spec_.cross_start_time_s;
  anim_speed_mult_ = spec_.base_mult > 0.0 ? spec_.base_mult : 1.0;
  active_ = line.size() >= 2;
  pose_ = ShowcasePose{};
  pose_.progress_m = start_p_ + 18.0;  // starts slightly ahead overhead for dramatic launch flyover
  smoothed_pitch_ = 0.0;
  smoothed_roll_ = 0.0;
  altitude_initialized_ = false;
  altitude_y_ = 0.0;
  if (active_) {
    double x, y, z, h;
    line_->pose(pose_.progress_m, 0.0, x, y, z, h);
    const double init_alt = std::max(min_clearance_m(), spec_.swoop_altitude_m) + 4.0;
    pose_.position = {x, y + init_alt, z};
    pose_.ground_y = y;
    pose_.agl_m = init_alt;
    pose_.heading = prev_heading_ = h;
    altitude_y_ = pose_.position[1];
  }
}

void ShowcaseOpponent::apply_initial_clearance(const GroundHeightFn& ground_height) {
  if (!active_ || !ground_height) return;
  const double px = pose_.position[0];
  const double pz = pose_.position[2];
  const double fwd_x = std::sin(pose_.heading);
  const double fwd_z = std::cos(pose_.heading);
  const double right_x = std::cos(pose_.heading);
  const double right_z = -std::sin(pose_.heading);
  double max_g = pose_.ground_y;
  double center_g = pose_.ground_y;
  bool have_center = false;
  for (double d : {0.0, 15.0, 30.0}) {
    for (double lat : {-6.0, 0.0, 6.0}) {
      if (auto gy = ground_height(px + fwd_x * d + right_x * lat, pz + fwd_z * d + right_z * lat);
          gy && std::isfinite(*gy)) {
        if (*gy > max_g) max_g = *gy;
        if (d == 0.0 && lat == 0.0) {
          center_g = std::max(center_g, *gy);
          have_center = true;
        }
      }
    }
  }
  const double min_y = max_g + min_clearance_m() + 2.0;
  if (pose_.position[1] < min_y) {
    pose_.position[1] = min_y;
    altitude_y_ = min_y;
  }
  if (have_center) pose_.ground_y = center_g;
  pose_.agl_m = std::max(0.0, pose_.position[1] - pose_.ground_y);
}

void ShowcaseOpponent::step(double dt, double race_clock, double player_progress, const GroundHeightFn& ground_height) {
  if (!active_ || !line_ || dt <= 0.0) return;

  // Spin propeller or rotor continuously (even during countdown).
  const double rotor_rate = spec_.kind == ShowcaseAircraftKind::kHotAirBalloon
                                ? 0.4
                                : (spec_.kind == ShowcaseAircraftKind::kMustangP51 ? 54.0 : 42.0);
  pose_.rotor_angle = std::fmod(pose_.rotor_angle + rotor_rate * dt, 2.0 * kPi);

  const double run_len = std::max(1.0, finish_p_ - start_p_);
  const double cruise_speed = run_len / std::max(1.0, nominal_duration_s_);
  const double finish_anim_t = use_xml_checkpoints_ ? cp_times_.back() : (spec_.cross_start_time_s + nominal_duration_s_);

  if (race_clock < 0.0) {
    if (spec_.on_countdown) {
      // Hot-Air Balloon (event 223): anim begins running during countdown.
      anim_time_ += anim_speed_mult_ * dt;
      const double frac = frac_from_anim_time(anim_time_);
      pose_.fraction = frac;
      pose_.progress_m = start_p_ + frac * run_len;
      pose_.speed_mps = cruise_speed * anim_speed_mult_;
    } else {
      // Countdown dramatic low flyover approaching the start gate.
      const double approach = std::clamp((race_clock + 6.0) / 6.0, 0.0, 1.0);
      const double p = std::max(0.0, start_p_ - 45.0 * (1.0 - approach) + 14.0 * approach);
      pose_.progress_m = p;
      pose_.fraction = 0.0;
      pose_.speed_mps = cruise_speed * 0.75;
    }
  } else if (!pose_.finished) {
    double extra_rb = 0.0;
    const double cur_frac = frac_from_anim_time(anim_time_, &extra_rb);
    const double player_frac = std::clamp((player_progress - start_p_) / run_len, 0.0, 1.0);

    // Spring rubber-banding from airborne_challenges.xml:
    // desired_mult = clamp(base_mult + spring * ((player_frac + offset) - plane_frac), min_mult, max_mult)
    // slewed smoothly by accel_rate (speeding up) and decel_rate (slowing down).
    const double rb_offset = spec_.rubber_band_offset_ahead + extra_rb;
    const double progress_diff = (player_frac + rb_offset) - cur_frac;
    const double min_m = std::max(0.1, spec_.rubber_band_min_mult);
    const double max_m = std::max(min_m, spec_.rubber_band_max_mult);
    const double base_m = std::clamp(spec_.base_mult > 0.0 ? spec_.base_mult : 1.0, min_m, max_m);
    const double desired_mult = std::clamp(base_m + spec_.rubber_band_spring * progress_diff, min_m, max_m);

    if (desired_mult > anim_speed_mult_) {
      const double step_up = std::max(0.01, spec_.rubber_band_accel_rate) * dt;
      anim_speed_mult_ = std::min(desired_mult, anim_speed_mult_ + step_up);
    } else {
      const double step_dn = std::max(0.01, spec_.rubber_band_decel_rate) * dt;
      anim_speed_mult_ = std::max(desired_mult, anim_speed_mult_ - step_dn);
    }

    const double prev_prog = pose_.progress_m;
    anim_time_ += anim_speed_mult_ * dt;
    const double next_frac = frac_from_anim_time(anim_time_);
    pose_.fraction = next_frac;
    pose_.progress_m = start_p_ + next_frac * run_len;
    pose_.speed_mps = std::max(0.0, (pose_.progress_m - prev_prog) / dt);

    if (anim_time_ >= finish_anim_t || pose_.progress_m >= finish_p_) {
      pose_.progress_m = finish_p_;
      pose_.fraction = 1.0;
      pose_.finished = true;
      pose_.finish_time = race_clock;
    }
  } else {
    // Victory/post-finish flypast beyond the finish line.
    pose_.progress_m = std::min(line_->length() - 5.0, pose_.progress_m + cruise_speed * 0.8 * dt);
    pose_.fraction = 1.0;
    pose_.speed_mps = cruise_speed * 0.8;
  }

  // Evaluate smooth woven horizontal flight path and forward heading tangent.
  auto woven_at = [&](double prog, double& ox, double& oroad_y, double& oz, double& oh) {
    const double pc = std::clamp(prog, 0.0, line_->length());
    double lx = 0.0, ly = 0.0, lz = 0.0, lh = 0.0;
    line_->pose(pc, 0.0, lx, ly, lz, lh);
    const double f = std::clamp((pc - start_p_) / run_len, 0.0, 1.0);
    const double w = spec_.lateral_sway_m * std::sin(f * 6.0 * kPi);
    const double rx = std::cos(lh), rz = -std::sin(lh);
    ox = lx + rx * w;
    oroad_y = ly;
    oz = lz + rz * w;
    oh = lh;
  };

  const double p_clamped = std::clamp(pose_.progress_m, 0.0, line_->length());
  double target_x = 0.0, road_y = 0.0, target_z = 0.0, road_h = 0.0;
  woven_at(p_clamped, target_x, road_y, target_z, road_h);
  double ahead_x = 0.0, ahead_road_y = 0.0, ahead_z = 0.0, ahead_h = 0.0;
  woven_at(std::min(line_->length(), p_clamped + 40.0), ahead_x, ahead_road_y, ahead_z, ahead_h);

  const double dx = ahead_x - target_x;
  const double dz = ahead_z - target_z;
  const double fly_h = std::hypot(dx, dz) > 1e-3 ? std::atan2(dx, dz) : road_h;
  const double dh = altitude_initialized_ ? wrap_angle(fly_h - prev_heading_) : 0.0;
  prev_heading_ = fly_h;

  // Terrain-clearance lookahead envelope:
  // Probe current XZ + forward stations (+15, +30, +50, +70, +100, +130, +200 m along heading)
  // with +/-6 m wingtip offsets on primary stations (0, 30, 70, 130, 200 m).
  const double fwd_x = std::sin(fly_h), fwd_z = std::cos(fly_h);
  const double right_x = std::cos(fly_h), right_z = -std::sin(fly_h);
  const double min_clear = min_clearance_m();
  const double lookahead_clear = min_clear + 1.5;

  auto sample_station = [&](double d, bool with_wingtips, double* out_center_y) -> double {
    double lx = 0.0, ly = 0.0, lz = 0.0, lh = 0.0;
    line_->pose(std::min(line_->length(), p_clamped + d), 0.0, lx, ly, lz, lh);
    double max_y = ly;
    double center_y = ly;
    if (ground_height) {
      const double cx = target_x + fwd_x * d;
      const double cz = target_z + fwd_z * d;
      if (auto gy = ground_height(cx, cz); gy && std::isfinite(*gy)) {
        if (*gy > max_y) max_y = *gy;
        if (*gy > center_y) center_y = *gy;
      }
      if (with_wingtips) {
        constexpr double kWingOffsetM = 6.0;
        for (double lat : {-kWingOffsetM, kWingOffsetM}) {
          if (auto gy = ground_height(cx + right_x * lat, cz + right_z * lat); gy && std::isfinite(*gy)) {
            if (*gy > max_y) max_y = *gy;
          }
        }
      }
    }
    if (out_center_y) *out_center_y = center_y;
    return max_y;
  };

  double center_ground_y = road_y;
  const double local_terrain_y = sample_station(0.0, true, &center_ground_y);

  struct LookaheadSample {
    double dist_m;
    double terrain_y;
  };
  const std::array<LookaheadSample, 7> lookahead = {{
      {15.0, sample_station(15.0, false, nullptr)},
      {30.0, sample_station(30.0, true, nullptr)},
      {50.0, sample_station(50.0, false, nullptr)},
      {70.0, sample_station(70.0, true, nullptr)},
      {100.0, sample_station(100.0, false, nullptr)},
      {130.0, sample_station(130.0, true, nullptr)},
      {200.0, sample_station(200.0, true, nullptr)},
  }};

  // Scenic swoop-and-climb envelope over local terrain (never below lookahead_clear on straights).
  const double frac = std::clamp((p_clamped - start_p_) / run_len, 0.0, 1.0);
  const double swoop_env = std::pow(std::sin(frac * 2.0 * kPi), 2.0);
  const double swoop_lo = std::max(lookahead_clear, spec_.swoop_altitude_m);
  const double swoop_hi = std::max(swoop_lo, spec_.base_altitude_m);
  double alt = swoop_lo + (swoop_hi - swoop_lo) * swoop_env;
  if (spec_.kind == ShowcaseAircraftKind::kHotAirBalloon) {
    alt += 3.0 * std::sin(race_clock * 0.7);
  }

  const double nominal_y = local_terrain_y + alt;
  const double local_floor_y = local_terrain_y + min_clear;

  if (!altitude_initialized_) {
    altitude_y_ = std::max({nominal_y, local_floor_y, lookahead[0].terrain_y + min_clear});
    smoothed_pitch_ = 0.0;
    smoothed_roll_ = 0.0;
    altitude_initialized_ = true;
  } else {
    const double horiz_speed = std::max({20.0, pose_.speed_mps, cruise_speed * 0.5});

    // Compute anticipatory ridge-climb slope across the entire lookahead envelope.
    double req_climb_slope = -1e9;
    for (const auto& st : lookahead) {
      const double req_y = st.terrain_y + lookahead_clear;
      // Arrive at clearance slightly ahead of each terrain peak so ridge crests clear smoothly.
      const double eff_d = std::max(8.0, st.dist_m - std::min(12.0, 0.35 * st.dist_m));
      const double slope = (req_y - altitude_y_) / eff_d;
      if (slope > req_climb_slope) req_climb_slope = slope;
    }

    const double nominal_slope = std::clamp((nominal_y - altitude_y_) / 70.0, -0.24, 0.35);
    const double target_slope = std::max(nominal_slope, req_climb_slope);
    const double clamped_slope = std::clamp(target_slope, -0.26, 0.58);
    const double desired_pitch = std::atan(clamped_slope);

    // Rate-limited smooth pitch damping so ridge climbs & swoops feel natural with zero snaps.
    const double pitch_alpha = 1.0 - std::exp(-dt / 0.25);
    const double max_dp = 1.6 * dt;
    const double dp = std::clamp((desired_pitch - smoothed_pitch_) * pitch_alpha, -max_dp, max_dp);
    smoothed_pitch_ = std::clamp(smoothed_pitch_ + dp, -0.28, 0.52);

    double vy = horiz_speed * std::tan(smoothed_pitch_);
    if (req_climb_slope > 0.0) {
      const double needed_vy = horiz_speed * std::min(req_climb_slope, 1.5);
      if (needed_vy > vy) {
        vy += (needed_vy - vy) * 0.65;
      }
    }
    altitude_y_ += vy * dt;

    // Hard minimum terrain clearance guarantee: aircraft never intersects mountains/ridges.
    if (altitude_y_ < local_floor_y) {
      altitude_y_ = local_floor_y;
    }
  }

  pose_.position = {target_x, altitude_y_, target_z};
  pose_.ground_y = center_ground_y;
  pose_.agl_m = std::max(0.0, altitude_y_ - center_ground_y);
  pose_.heading = fly_h;

  const double yaw_rate = dh / std::max(0.01, dt);
  if (spec_.kind == ShowcaseAircraftKind::kHotAirBalloon) {
    pose_.pitch = 0.0;
    pose_.roll = 0.06 * std::sin(race_clock * 0.9);
  } else if (spec_.kind == ShowcaseAircraftKind::kHelicopter) {
    pose_.pitch = std::clamp(-0.14 + smoothed_pitch_ * 0.45, -0.24, 0.16);
    const double target_roll = std::clamp(-yaw_rate * 0.40, -0.45, 0.45);
    smoothed_roll_ += (target_roll - smoothed_roll_) * (1.0 - std::exp(-dt / 0.20));
    pose_.roll = smoothed_roll_;
  } else {
    pose_.pitch = smoothed_pitch_;
    // Negative roll dips the +X right wing into right turns (dh > 0); positive roll banks left.
    const double bank_from_turn = std::clamp(-yaw_rate * 0.65, -0.68, 0.68);
    const double bank_from_weave = 0.24 * std::sin(frac * 6.0 * kPi);
    const double target_roll = std::clamp(bank_from_turn + bank_from_weave, -0.78, 0.78);
    smoothed_roll_ += (target_roll - smoothed_roll_) * (1.0 - std::exp(-dt / 0.20));
    pose_.roll = smoothed_roll_;
  }
}

int ShowcaseOpponent::player_place(double player_progress, bool player_finished, double player_finish_time) const {
  if (!active_) return 1;
  if (player_finished && pose_.finished) return player_finish_time <= pose_.finish_time ? 1 : 2;
  if (player_finished) return 1;
  if (pose_.finished) return 2;
  return player_progress >= pose_.progress_m ? 1 : 2;
}

double ShowcaseOpponent::near_rumble_strength(double dist_m) const {
  if (!active_ || spec_.rumble_strength <= 0.0) return 0.0;
  if (dist_m >= spec_.rumble_distance_outer_m) return 0.0;
  if (dist_m <= spec_.rumble_distance_inner_m) return spec_.rumble_strength;
  const double span = std::max(1e-3, spec_.rumble_distance_outer_m - spec_.rumble_distance_inner_m);
  const double t = 1.0 - (dist_m - spec_.rumble_distance_inner_m) / span;
  return spec_.rumble_strength * std::clamp(t, 0.0, 1.0);
}

}  // namespace race
