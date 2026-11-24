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
      s.base_altitude_m = 22.0;
      s.swoop_altitude_m = 12.0;
      s.lateral_sway_m = 15.0;
      s.target_speed_mps = 50.0;
      s.scale = 1.45;
      break;
    case ShowcaseAircraftKind::kHelicopter:
      s.display_name = "HELICOPTER";
      s.base_altitude_m = 19.0;
      s.swoop_altitude_m = 9.5;
      s.lateral_sway_m = 12.0;
      s.target_speed_mps = 48.0;
      s.scale = 1.2;
      break;
    case ShowcaseAircraftKind::kBiplane:
      s.display_name = "BIPLANE";
      s.base_altitude_m = 21.0;
      s.swoop_altitude_m = 9.5;
      s.lateral_sway_m = 14.0;
      s.target_speed_mps = 46.0;
      s.scale = 1.15;
      break;
    case ShowcaseAircraftKind::kHotAirBalloon:
      s.display_name = "HOT-AIR BALLOON";
      s.base_altitude_m = 36.0;
      s.swoop_altitude_m = 18.0;
      s.lateral_sway_m = 10.0;
      s.target_speed_mps = 44.0;
      s.scale = 1.35;
      break;
    case ShowcaseAircraftKind::kStuntJet:
    default:
      s.display_name = "SHOWCASE AIRCRAFT";
      s.base_altitude_m = 24.0;
      s.swoop_altitude_m = 10.0;
      s.lateral_sway_m = 15.0;
      s.target_speed_mps = 48.0;
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
  if (active_) {
    double x, y, z, h;
    line_->pose(pose_.progress_m, 0.0, x, y, z, h);
    pose_.position = {x, y + spec_.swoop_altitude_m + 4.0, z};
    pose_.heading = prev_heading_ = h;
  }
}

void ShowcaseOpponent::step(double dt, double race_clock, double player_progress) {
  if (!active_ || !line_ || dt <= 0.0) return;

  // Spin propeller or rotor continuously (even during countdown).
  const double rotor_rate = spec_.kind == ShowcaseAircraftKind::kHotAirBalloon ? 0.4 : 42.0;
  pose_.rotor_angle = std::fmod(pose_.rotor_angle + rotor_rate * dt, 2.0 * kPi);

  const double run_len = std::max(1.0, finish_p_ - start_p_);
  const double finish_anim_t = use_xml_checkpoints_ ? cp_times_.back() : (spec_.cross_start_time_s + nominal_duration_s_);

  if (race_clock < 0.0) {
    if (spec_.on_countdown) {
      // Hot-Air Balloon (event 223): anim begins running during countdown.
      anim_time_ += anim_speed_mult_ * dt;
      const double frac = frac_from_anim_time(anim_time_);
      pose_.fraction = frac;
      pose_.progress_m = start_p_ + frac * run_len;
      pose_.speed_mps = (run_len / std::max(1.0, nominal_duration_s_)) * anim_speed_mult_;
    } else {
      // Countdown dramatic low flyover approaching the start gate.
      const double approach = std::clamp((race_clock + 6.0) / 6.0, 0.0, 1.0);
      const double p = std::max(0.0, start_p_ - 45.0 * (1.0 - approach) + 14.0 * approach);
      pose_.progress_m = p;
      pose_.fraction = 0.0;
      pose_.speed_mps = (run_len / std::max(1.0, nominal_duration_s_)) * 0.75;
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
    const double cruise = run_len / std::max(1.0, nominal_duration_s_);
    pose_.progress_m = std::min(line_->length() - 5.0, pose_.progress_m + cruise * 0.8 * dt);
    pose_.fraction = 1.0;
  }

  const double p_clamped = std::clamp(pose_.progress_m, 0.0, line_->length());
  double x = 0, y = 0, z = 0, h = 0;
  line_->pose(p_clamped, 0.0, x, y, z, h);
  double xf = 0, yf = 0, zf = 0, hf = 0;
  line_->pose(std::min(line_->length(), p_clamped + 45.0), 0.0, xf, yf, zf, hf);

  // Smooth lateral S-curve weave & swoop altitude profile.
  const double frac = std::clamp((p_clamped - start_p_) / run_len, 0.0, 1.0);
  const double weave = spec_.lateral_sway_m * std::sin(frac * 6.0 * kPi);
  const double swoop_env = std::pow(std::sin(frac * 2.0 * kPi), 2.0);
  double alt = spec_.swoop_altitude_m + (spec_.base_altitude_m - spec_.swoop_altitude_m) * swoop_env;
  if (spec_.kind == ShowcaseAircraftKind::kHotAirBalloon) {
    alt += 3.0 * std::sin(race_clock * 0.7);
  }

  const double rx = std::cos(h), rz = -std::sin(h);
  const double target_x = x + rx * weave;
  const double target_z = z + rz * weave;
  const double target_y = std::max(y, yf) + alt;

  const double dx = xf - x, dz = zf - z;
  const double fly_h = std::hypot(dx, dz) > 1e-3 ? std::atan2(dx, dz) : h;
  const double dh = wrap_angle(fly_h - prev_heading_);
  prev_heading_ = fly_h;

  pose_.position = {target_x, target_y, target_z};
  pose_.heading = fly_h;
  if (spec_.kind == ShowcaseAircraftKind::kHotAirBalloon) {
    pose_.pitch = 0.0;
    pose_.roll = 0.06 * std::sin(race_clock * 0.9);
  } else if (spec_.kind == ShowcaseAircraftKind::kHelicopter) {
    pose_.pitch = -0.14;  // forward nose-down hover tilt
    pose_.roll = std::clamp(dh / std::max(0.01, dt) * 0.35, -0.45, 0.45);
  } else {
    pose_.pitch = std::clamp((yf - y) / 45.0, -0.25, 0.25);
    const double bank_from_turn = std::clamp(dh / std::max(0.01, dt) * 0.55, -0.65, 0.65);
    const double bank_from_weave = -0.28 * std::cos(frac * 6.0 * kPi);
    pose_.roll = std::clamp(bank_from_turn + bank_from_weave, -0.75, 0.75);
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
