#include "freeroam/club.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <cstring>

#include "audio/xml.h"
#include "fh1data/archive.h"
#include "fh1data/stringtables.h"

namespace freeroam::club {

namespace {

bool fail(std::string* error, const std::string& what) {
  if (error) *error = what;
  return false;
}

std::string lower(std::string_view sv) {
  std::string s(sv);
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string title_case_words(std::string_view sv) {
  std::string out;
  out.reserve(sv.size());
  bool cap = true;
  for (char ch : sv) {
    if (ch == '_' || ch == '-') {
      out.push_back(' ');
      cap = true;
    } else if (cap) {
      out.push_back(char(std::toupper(static_cast<unsigned char>(ch))));
      cap = false;
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

std::string location_display_name(std::string_view raw) {
  const std::string lo = lower(raw);
  static const std::pair<const char*, const char*> kKnown[] = {
      {"outpost_redrock", "Red Rock Outpost"},       {"outpost_red_rock", "Red Rock Outpost"},
      {"outpost_beaumont", "Beaumont Outpost"},      {"outpost_carson", "Carson Outpost"},
      {"outpost_clear_springs", "Clear Springs Outpost"}, {"outpost_clearsprings", "Clear Springs Outpost"},
      {"outpost_bunkers", "Bunker Outpost"},         {"outpost_dam", "Finley Dam Outpost"},
      {"outpost_finley", "Finley Dam Outpost"},      {"outpost_golf", "Golf Course Outpost"},
      {"outpost_montano", "Montano Plains Outpost"}, {"outpost_eagle", "Eagle Ridge Outpost"},
      {"outpost_dak", "Dak's Outpost"},              {"festival", "Horizon Festival"},
      {"racecentral", "Race Central"},               {"redrock", "Red Rock"},
      {"beaumont", "Beaumont"},                      {"carson", "Carson"},
      {"clearsprings", "Clear Springs"},             {"montano", "Montano Plains"},
  };
  for (const auto& [needle, label] : kKnown) {
    if (lo.find(needle) != std::string::npos) return label;
  }
  std::string clean(raw);
  auto strip_pfx = [&](const char* pfx) {
    const size_t n = std::strlen(pfx);
    if (clean.size() > n && lower(clean.substr(0, n)) == pfx) clean.erase(0, n);
  };
  strip_pfx("outpost_");
  strip_pfx("gasstation_");
  strip_pfx("loc_");
  for (const char* suf : {"_node", "_trigger", "_zone", "_area", "_loc"}) {
    const size_t n = std::strlen(suf);
    if (clean.size() > n && lower(clean.substr(clean.size() - n)) == suf) clean.resize(clean.size() - n);
  }
  return title_case_words(clean);
}

std::string format_number(double v) {
  char buf[64];
  if (std::fabs(v - std::round(v)) < 1e-6) {
    std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(std::llround(v)));
  } else {
    std::snprintf(buf, sizeof buf, "%g", v);
  }
  return buf;
}

bool parse_cmp(const std::string& s, Cmp& out) {
  static const std::pair<const char*, Cmp> kNames[] = {
      {"equal_to", Cmp::kEqual},     {"not_equal_to", Cmp::kNotEqual}, {"more_than", Cmp::kMore},
      {"more_equal", Cmp::kMoreEqual}, {"less_than", Cmp::kLess},      {"less_equal", Cmp::kLessEqual}};
  for (const auto& [name, c] : kNames)
    if (audio::iequals(s, name)) return out = c, true;
  return false;
}

const char* cmp_text(Cmp c) {
  switch (c) {
    case Cmp::kEqual: return "=";
    case Cmp::kNotEqual: return "!=";
    case Cmp::kMore: return ">";
    case Cmp::kMoreEqual: return ">=";
    case Cmp::kLess: return "<";
    case Cmp::kLessEqual: return "<=";
  }
  return "?";
}

bool compare(double a, Cmp c, double b) {
  switch (c) {
    case Cmp::kEqual: return a == b;
    case Cmp::kNotEqual: return a != b;
    case Cmp::kMore: return a > b;
    case Cmp::kMoreEqual: return a >= b;
    case Cmp::kLess: return a < b;
    case Cmp::kLessEqual: return a <= b;
  }
  return false;
}

int parse_trailing_camera_id(std::string_view name) {
  const size_t pos = name.rfind('_');
  if (pos == std::string_view::npos || pos + 1 >= name.size()) return 0;
  for (size_t i = pos + 1; i < name.size(); ++i)
    if (!std::isdigit(static_cast<unsigned char>(name[i]))) return 0;
  return std::atoi(std::string(name.substr(pos + 1)).c_str());
}

bool is_known_skill_type(std::string_view t) {
  static const char* kSkills[] = {
      "air",          "burnout",        "cleanspeed", "daredevil", "destruction", "draft",
      "drift",        "edrift",         "esideswipe", "jturn",     "nearmiss",    "quickoffthemark",
      "sideswipe",    "tradingpaint",   "turn180",    "weaver",
  };
  for (const char* k : kSkills)
    if (t == k) return true;
  return false;
}

bool is_known_smashable_type(std::string_view t) {
  if (t.empty()) return true;
  static const char* kTypes[] = {"sign", "fence", "pole", "camera", "picnic", "cone", "flower", "bin"};
  for (const char* k : kTypes)
    if (t == k) return true;
  return false;
}

bool is_known_hub(std::string_view h) {
  return h == "gladstone" || h == "carson" || h == "redrock";
}

bool is_known_event_style(std::string_view st) {
  return st == "none" || st == "street" || st == "festival";
}

std::string skill_display_name(std::string_view type, int min_grade, int max_grade) {
  std::string base;
  if (type == "air") base = "Air";
  else if (type == "burnout") base = "Burnout";
  else if (type == "cleanspeed") base = "Speed";
  else if (type == "daredevil") base = "Daredevil";
  else if (type == "destruction") base = "Wreckage";
  else if (type == "draft") base = "Draft";
  else if (type == "drift") base = "Drift";
  else if (type == "edrift") base = "E-Drift";
  else if (type == "esideswipe") base = "E-Sideswipe";
  else if (type == "jturn") base = "J-Turn";
  else if (type == "nearmiss") base = "Near Miss";
  else if (type == "quickoffthemark") base = "Quick Off the Mark";
  else if (type == "sideswipe") base = "Sideswipe";
  else if (type == "tradingpaint") base = "Trading Paint";
  else if (type == "turn180") base = "180";
  else if (type == "weaver") base = "Weaver";
  else base = title_case_words(type);

  if (min_grade == 0 && max_grade == 0) return "Ultimate " + base;
  if (min_grade == 1) return "Awesome " + base;
  if (min_grade == 2) return "Great " + base;
  return base;
}

std::string camera_display_name(std::string_view raw) {
  std::string s(raw);
  const size_t pos = s.rfind('_');
  if (pos != std::string::npos && parse_trailing_camera_id(s) > 0) s.resize(pos);
  return title_case_words(s);
}

std::optional<std::string> lookup_str(const fh1::StringTables* tables, std::string_view table, std::string_view id) {
  if (!tables || id.empty()) return std::nullopt;
  const uint32_t packed = (uint32_t(fh1::string_hash(table)) << 16) | uint32_t(fh1::string_hash(id));
  return tables->resolve(std::to_string(packed));
}

// Deterministic 32-bit FNV-1a over the parsed definition tree.
struct Hasher {
  uint32_t h = 2166136261u;
  void byte(uint8_t b) {
    h ^= b;
    h *= 16777619u;
  }
  void i64(int64_t v) {
    for (int i = 0; i < 8; ++i) byte(uint8_t((uint64_t(v) >> (i * 8)) & 0xFFu));
  }
  void dbl(double v) { i64(std::llround(v * 1000.0)); }
  void str(std::string_view s) {
    for (char ch : s) byte(uint8_t(ch));
    byte(0xFFu);
  }
  void cond(const Condition& c) {
    byte(uint8_t(c.kind));
    byte(uint8_t(c.cmp));
    dbl(c.value);
    byte(c.inside ? 1 : 0);
    str(c.name);
    i64(c.min_grade);
    i64(c.max_grade);
    byte(c.camera_zone ? 1 : 0);
    i64(c.camera_id);
    dbl(c.min_speed);
    dbl(c.max_speed);
    byte(c.unique ? 1 : 0);
    i64(c.min_finish_place);
    str(c.mode);
  }
  void node(const Node& n) {
    byte(uint8_t(n.kind));
    i64(n.pass_count);
    dbl(n.pass_interval);
    byte(n.save_progress ? 1 : 0);
    str(n.start_loc);
    str(n.end_loc);
    i64(int64_t(n.conditions.size()));
    for (const auto& c : n.conditions) cond(c);
    i64(int64_t(n.children.size()));
    for (const auto& ch : n.children) node(ch);
  }
};

struct Parser {
  Challenge* c;

  void unsupported(const std::string& what) {
    if (c->supported) c->supported = false, c->unsupported = what;
  }

  void collect_desc_params(const audio::XmlNode& e, const std::string& tag, const Condition* cond = nullptr) {
    for (int idx = 0; idx < 5; ++idx) {
      const std::string key = "desc_param" + std::to_string(idx);
      if (!e.has(key)) continue;
      const std::string attr = lower(e.attr(key));
      DescParam p;
      if (tag == "skills" && attr == "type") {
        p.kind = DescParam::Kind::kSkill;
        p.raw = cond ? cond->name : lower(e.attr("type"));
        p.min_grade = cond ? cond->min_grade : 3;
        p.max_grade = cond ? cond->max_grade : 0;
      } else if ((tag == "a_to_b" && (attr == "start_loc" || attr == "end_loc")) ||
                 (tag == "location" && attr == "loc_name")) {
        p.kind = DescParam::Kind::kLocation;
        p.raw = lower(e.attr(attr));
      } else if (tag == "speed_camera" && attr == "name") {
        p.kind = DescParam::Kind::kSpeedCamera;
        p.raw = lower(e.attr("name"));
      } else if (tag == "event_id" && attr == "name") {
        p.kind = DescParam::Kind::kEventId;
        p.event_id = int64_t(std::llround(e.number("value")));
        p.raw = std::to_string(p.event_id);
      } else if (attr == "pass_count" || attr == "pass_interval" || attr == "value" || attr == "min_speed" ||
                 attr == "max_speed") {
        p.kind = DescParam::Kind::kNumber;
        p.number = e.number(attr);
        p.raw = format_number(p.number);
      } else {
        p.kind = DescParam::Kind::kLocation;
        p.raw = e.attr(attr);
      }
      c->desc_params[size_t(idx)] = std::move(p);
    }
  }

  Condition condition(const audio::XmlNode& e) {
    Condition k;
    const std::string tag = lower(e.name);
    k.name = tag;
    static const std::pair<const char*, Cond> kKinds[] = {
        {"speed", Cond::kSpeed},
        {"timer", Cond::kTimer},
        {"gear", Cond::kGear},
        {"in_air", Cond::kInAir},
        {"is_drifting", Cond::kIsDrifting},
        {"distance", Cond::kDistance},
        {"location", Cond::kLocation},
        {"collided", Cond::kCollided},
        {"skills", Cond::kSkills},
        {"skill_points", Cond::kSkillPoints},
        {"num_skill_chains", Cond::kNumSkillChains},
        {"smashables", Cond::kSmashables},
        {"speed_camera", Cond::kSpeedCamera},
        {"event_started", Cond::kEventStarted},
        {"event_restarted", Cond::kEventRestarted},
        {"event_completed", Cond::kEventCompleted},
        {"event_id", Cond::kEventId},
        {"event_hub", Cond::kEventHub},
        {"career_event_style", Cond::kCareerEventStyle},
        {"encounter_started", Cond::kEncounterStarted},
        {"encounter_completed", Cond::kEncounterCompleted},
        {"race_progress", Cond::kRaceProgress},
    };
    for (const auto& [name, kind] : kKinds)
      if (tag == name) k.kind = kind;
    if (k.kind == Cond::kUnsupported) {
      unsupported(tag);
      return k;
    }

    switch (k.kind) {
      case Cond::kSpeed:
      case Cond::kTimer:
      case Cond::kGear:
      case Cond::kDistance:
      case Cond::kSkillPoints:
      case Cond::kNumSkillChains:
      case Cond::kEventId:
      case Cond::kRaceProgress:
        if (!parse_cmp(e.attr("is"), k.cmp) || !e.has("value")) unsupported(tag + " without is/value");
        k.value = e.number("value");
        if (k.kind == Cond::kRaceProgress) k.mode = lower(e.attr("mode"));
        break;
      case Cond::kLocation:
        k.inside = !audio::iequals(e.attr("mode", "inside"), "outside");
        k.name = lower(e.attr("loc_name"));
        if (k.name.empty()) unsupported("location without loc_name");
        break;
      case Cond::kSkills:
        k.name = lower(e.attr("type"));
        if (!is_known_skill_type(k.name)) unsupported("unknown skill " + k.name);
        k.min_grade = e.has("min_grade") ? std::clamp(int(std::llround(e.number("min_grade", 3))), 0, 3) : 3;
        // Ignore unknown typo `max_grad` per SPEC-V4-CONTENT; only honour `max_grade`.
        k.max_grade = e.has("max_grade") ? std::clamp(int(std::llround(e.number("max_grade", 0))), 0, 3) : 0;
        if (k.max_grade > k.min_grade) std::swap(k.max_grade, k.min_grade);
        break;
      case Cond::kSmashables:
        k.name = lower(e.attr("type"));
        if (!is_known_smashable_type(k.name)) unsupported("unknown smashable " + k.name);
        break;
      case Cond::kSpeedCamera: {
        const std::string t = lower(e.attr("type", "trap"));
        if (t != "trap" && t != "zone") unsupported("speed_camera type " + t);
        k.camera_zone = (t == "zone");
        k.name = lower(e.attr("name"));
        k.camera_id = parse_trailing_camera_id(k.name);
        k.min_speed = e.has("min_speed") ? e.number("min_speed") : 0.0;
        k.max_speed = e.has("max_speed") ? e.number("max_speed") : 0.0;
        k.unique = e.has("unique") && !audio::iequals(e.attr("unique"), "false");
        break;
      }
      case Cond::kEventCompleted:
      case Cond::kEncounterCompleted:
        k.min_finish_place = std::max(1, int(std::llround(e.number("min_finish_place", 1))));
        break;
      case Cond::kEventHub:
        k.name = lower(e.attr("id"));
        if (!is_known_hub(k.name)) unsupported("unknown event_hub " + k.name);
        break;
      case Cond::kCareerEventStyle:
        k.name = lower(e.attr("id"));
        if (!is_known_event_style(k.name)) unsupported("unknown career_event_style " + k.name);
        break;
      case Cond::kInAir:
      case Cond::kIsDrifting:
      case Cond::kCollided:
      case Cond::kEventStarted:
      case Cond::kEventRestarted:
      case Cond::kEncounterStarted:
      case Cond::kUnsupported:
        break;
    }

    collect_desc_params(e, tag, &k);
    return k;
  }

  Node node(const audio::XmlNode& e) {
    Node n;
    const std::string name = lower(e.name);
    if (name == "pass_when") n.kind = Kind::kPass;
    else if (name == "fail_when") n.kind = Kind::kFail;
    else if (name == "fail_unless") n.kind = Kind::kFailUnless;
    else if (name == "a_to_b") n.kind = Kind::kAToB;
    else if (name == "group") {
      const std::string type = lower(e.attr("type", "and"));
      n.kind = type == "then" ? Kind::kThen : type == "or" ? Kind::kOr : Kind::kAnd;
      if (type != "then" && type != "or" && type != "and") unsupported("group type " + type);
    } else {
      unsupported(name);
      return n;
    }
    n.pass_count = std::max(1, int(e.number("pass_count", 1)));
    n.pass_interval = e.number("pass_interval", 0);
    n.save_progress = e.has("save_progress") && !audio::iequals(e.attr("save_progress"), "false");
    n.start_loc = lower(e.attr("start_loc"));
    n.end_loc = lower(e.attr("end_loc"));
    collect_desc_params(e, name);
    for (const auto& k : e.children) {
      if (audio::iequals(k.name, "hud")) continue;
      if (n.kind == Kind::kPass || n.kind == Kind::kFail || n.kind == Kind::kFailUnless)
        n.conditions.push_back(condition(k));
      else
        n.children.push_back(node(k));
    }
    return n;
  }
};

std::string describe(const Node& n) {
  auto cond = [](const Condition& c) -> std::string {
    char b[128];
    switch (c.kind) {
      case Cond::kSpeed: std::snprintf(b, sizeof b, "speed %s %g mph", cmp_text(c.cmp), c.value); return b;
      case Cond::kTimer: std::snprintf(b, sizeof b, "%s %g s", cmp_text(c.cmp), c.value); return b;
      case Cond::kGear: std::snprintf(b, sizeof b, "gear %s %g", cmp_text(c.cmp), c.value); return b;
      case Cond::kDistance: std::snprintf(b, sizeof b, "jump %s %g m", cmp_text(c.cmp), c.value); return b;
      case Cond::kInAir: return "in the air";
      case Cond::kIsDrifting: return "drifting";
      case Cond::kCollided: return "a crash";
      case Cond::kLocation: return std::string(c.inside ? "in " : "out of ") + location_display_name(c.name);
      case Cond::kSkills: return skill_display_name(c.name, c.min_grade, c.max_grade);
      case Cond::kSkillPoints:
        std::snprintf(b, sizeof b, "skill chain %s %g pts", cmp_text(c.cmp), c.value);
        return b;
      case Cond::kNumSkillChains:
        std::snprintf(b, sizeof b, "skill chains %s %g", cmp_text(c.cmp), c.value);
        return b;
      case Cond::kSmashables: return c.name.empty() ? "smash object" : "smash " + c.name;
      case Cond::kSpeedCamera: {
        std::string s = c.camera_zone ? "speed zone" : "speed trap";
        if (!c.name.empty()) s += " " + camera_display_name(c.name);
        if (c.min_speed > 0 && c.max_speed > 0) {
          std::snprintf(b, sizeof b, " %g-%g mph", c.min_speed, c.max_speed);
          s += b;
        } else if (c.min_speed > 0) {
          std::snprintf(b, sizeof b, " >= %g mph", c.min_speed);
          s += b;
        }
        return s;
      }
      case Cond::kEventStarted: return "start event";
      case Cond::kEventRestarted: return "restart event";
      case Cond::kEventCompleted:
        std::snprintf(b, sizeof b, "finish top %d", c.min_finish_place);
        return b;
      case Cond::kEventId:
        std::snprintf(b, sizeof b, "event %s %g", cmp_text(c.cmp), c.value);
        return b;
      case Cond::kEventHub: return "hub " + c.name;
      case Cond::kCareerEventStyle: return "style " + c.name;
      case Cond::kEncounterStarted: return "start head-to-head";
      case Cond::kEncounterCompleted:
        std::snprintf(b, sizeof b, "win head-to-head (top %d)", c.min_finish_place);
        return b;
      case Cond::kRaceProgress:
        std::snprintf(b, sizeof b, "race progress %s %g", cmp_text(c.cmp), c.value);
        return b;
      case Cond::kUnsupported: return c.name;
    }
    return {};
  };
  auto conds = [&](const Node& k) {
    std::string s;
    for (const auto& c : k.conditions) s += (s.empty() ? "" : " and ") + cond(c);
    return s;
  };
  switch (n.kind) {
    case Kind::kPass: {
      std::string s = conds(n);
      if (n.pass_count > 1) s += " x" + std::to_string(n.pass_count);
      if (n.pass_interval > 0) s += " in " + format_number(n.pass_interval) + "s";
      return s;
    }
    case Kind::kFail: return "fails on " + conds(n);
    case Kind::kFailUnless: return "while " + conds(n);
    case Kind::kAToB: {
      std::string s = title_case_words(n.start_loc) + " to " + title_case_words(n.end_loc);
      for (const auto& k : n.children) s += ", " + describe(k);
      return s;
    }
    case Kind::kAnd:
    case Kind::kThen:
    case Kind::kOr: {
      std::string s;
      const char* sep = n.kind == Kind::kThen ? ", then " : n.kind == Kind::kOr ? " or " : ", ";
      for (const auto& k : n.children)
        s += (s.empty() ? "" : (k.kind == Kind::kFail || k.kind == Kind::kFailUnless) ? ", " : sep) + describe(k);
      if (n.pass_count > 1) s += " x" + std::to_string(n.pass_count);
      return n.children.size() > 1 && n.kind != Kind::kAnd ? "(" + s + ")" : s;
    }
  }
  return {};
}

bool is_fail(const Node& n) { return n.kind == Kind::kFail || n.kind == Kind::kFailUnless; }

bool is_event_cond(Cond k) {
  switch (k) {
    case Cond::kSkills:
    case Cond::kSmashables:
    case Cond::kSpeedCamera:
    case Cond::kEventStarted:
    case Cond::kEventRestarted:
    case Cond::kEventCompleted:
    case Cond::kEncounterStarted:
    case Cond::kEncounterCompleted:
      return true;
    default:
      return false;
  }
}

void check_locations(const Node& n, const Locations& locs, std::string& missing) {
  auto need = [&](const std::string& name) {
    if (!name.empty() && !locs.count(name) && missing.empty()) missing = "location " + name;
  };
  if (n.kind == Kind::kAToB) need(n.start_loc), need(n.end_loc);
  for (const auto& c : n.conditions)
    if (c.kind == Cond::kLocation) need(c.name);
  for (const auto& k : n.children) check_locations(k, locs, missing);
}

bool camera_matches_target(const Condition& c, const SpeedCameraHit& h) {
  if (h.is_zone != c.camera_zone) return false;
  if (!c.name.empty()) {
    const bool id_match = (c.camera_id > 0 && h.scoreboard_id == c.camera_id);
    const bool name_match = (!h.name.empty() && lower(h.name) == c.name);
    if (!id_match && !name_match) return false;
  }
  return true;
}

bool camera_matches_speed(const Condition& c, const SpeedCameraHit& h) {
  if (c.min_speed > 0 && h.speed_mph < c.min_speed) return false;
  if (c.max_speed > 0 && h.speed_mph > c.max_speed) return false;
  return true;
}

std::string camera_hit_key(const SpeedCameraHit& h) {
  if (h.scoreboard_id > 0) return (h.is_zone ? "z:" : "t:") + std::to_string(h.scoreboard_id);
  if (!h.name.empty()) return (h.is_zone ? "zn:" : "tn:") + lower(h.name);
  return h.is_zone ? "zone" : "trap";
}

// Non-negative integer encoding for camera keys so `seen_cameras` and `camera_hits`
// round-trip cleanly through `DriveSave::SavedClubCar::progress` (vector<int> >= 0).
int encode_camera_key(const std::string& key) {
  if (key.rfind("t:", 0) == 0) {
    const int id = std::atoi(key.c_str() + 2);
    if (id > 0 && id < 0x1FFFFFFF) return (id << 2) | 0;
  } else if (key.rfind("z:", 0) == 0) {
    const int id = std::atoi(key.c_str() + 2);
    if (id > 0 && id < 0x1FFFFFFF) return (id << 2) | 1;
  } else if (key.rfind("h:", 0) == 0) {
    const int tok = std::atoi(key.c_str() + 2);
    if (tok > 0) return tok;
  }
  uint32_t h = 2166136261u;
  for (char ch : key) {
    h ^= uint8_t(ch);
    h *= 16777619u;
  }
  const int hash29 = int((h & 0x1FFFFFFFu) ? (h & 0x1FFFFFFFu) : 1u);
  return (hash29 << 2) | 2;
}

std::string decode_camera_key(int tok) {
  if (tok <= 0) return {};
  const int kind = tok & 3;
  const int val = tok >> 2;
  if (kind == 0) return "t:" + std::to_string(val);
  if (kind == 1) return "z:" + std::to_string(val);
  return "h:" + std::to_string(tok);
}

std::string canonical_camera_key(const SpeedCameraHit& h) {
  return decode_camera_key(encode_camera_key(camera_hit_key(h)));
}

bool tree_has_chain_fail(const Node& n) {
  if (is_fail(n)) {
    for (const auto& c : n.conditions)
      if (c.kind == Cond::kNumSkillChains) return true;
  }
  for (const auto& ch : n.children)
    if (tree_has_chain_fail(ch)) return true;
  return false;
}

const Condition* find_speed_camera_cond(const Node& n) {
  if (n.kind != Kind::kPass) return nullptr;
  for (const auto& c : n.conditions)
    if (c.kind == Cond::kSpeedCamera) return &c;
  return nullptr;
}

const Condition* find_skill_points_cond(const Node& n) {
  if (n.kind != Kind::kPass) return nullptr;
  for (const auto& c : n.conditions)
    if (c.kind == Cond::kSkillPoints) return &c;
  return nullptr;
}

bool has_substr(std::string_view hay, std::string_view needle) {
  return hay.find(needle) != std::string_view::npos;
}

}  // namespace

std::string classify_smashable_type(std::string_view prop_stem, int8_t object_type_index) {
  const std::string s = lower(prop_stem);
  if (has_substr(s, "fence") || has_substr(s, "railing") || has_substr(s, "barrier") || has_substr(s, "gate"))
    return "fence";
  if (has_substr(s, "sign") || has_substr(s, "flyer") || has_substr(s, "board") || has_substr(s, "poster") ||
      has_substr(s, "billboard") || has_substr(s, "milepost"))
    return "sign";
  if (has_substr(s, "camera") || has_substr(s, "tripod") || has_substr(s, "photo") || has_substr(s, "speedtrap"))
    return "camera";
  if (has_substr(s, "picnic") || has_substr(s, "table") || has_substr(s, "bench") || has_substr(s, "chair") ||
      has_substr(s, "umbrella") || has_substr(s, "tent") || has_substr(s, "gazebo"))
    return "picnic";
  if (has_substr(s, "cone") || has_substr(s, "marker") || has_substr(s, "delineator"))
    return "cone";
  if (has_substr(s, "flower") || has_substr(s, "plant") || has_substr(s, "pot") || has_substr(s, "bush") ||
      has_substr(s, "shrub") || has_substr(s, "cactus") || has_substr(s, "planter"))
    return "flower";
  if (has_substr(s, "bin") || has_substr(s, "trash") || has_substr(s, "dumpster") || has_substr(s, "garbage") ||
      has_substr(s, "barrel") || has_substr(s, "crate") || has_substr(s, "can"))
    return "bin";
  if (has_substr(s, "pole") || has_substr(s, "lamp") || has_substr(s, "light") || has_substr(s, "post") ||
      has_substr(s, "hydrant") || has_substr(s, "mailbox") || has_substr(s, "bollard") || has_substr(s, "pylon"))
    return "pole";
  if (object_type_index == 0) return "fence";
  if (object_type_index == 1) return "sign";
  if (object_type_index == 2) return "pole";
  return "generic";
}

int skill_tracker_grade_to_club_grade(int tracker_grade) {
  return 4 - std::clamp(tracker_grade, 1, 4);
}

std::string Challenge::label() const {
  std::string s = title;
  if (s.rfind("IDS_", 0) == 0) s = s.substr(4);
  s = lower(s);
  for (char& ch : s)
    if (ch == '_' || ch == '-') ch = ' ';
  return s;
}

std::string Challenge::summary() const { return describe(root); }

std::string Challenge::format_title(const fh1::StringTables* strings) const {
  if (auto s = lookup_str(strings, "carchallengetitles", title); s && !s->empty()) return *s;
  if (auto s = lookup_str(strings, "carchallenges", title); s && !s->empty()) return *s;
  return title_case_words(label());
}

std::string Challenge::format_description(const fh1::StringTables* strings,
                                          const std::map<int64_t, std::string>* event_names) const {
  std::string tmpl;
  if (auto s1 = lookup_str(strings, "carchallengedescriptions", description); s1 && !s1->empty()) {
    tmpl = *s1;
  } else if (auto s2 = lookup_str(strings, "carchallenges", description); s2 && !s2->empty()) {
    tmpl = *s2;
  } else {
    return summary();
  }

  auto resolve_param = [&](const DescParam& p) -> std::string {
    switch (p.kind) {
      case DescParam::Kind::kNone:
        return {};
      case DescParam::Kind::kNumber:
        return p.raw;
      case DescParam::Kind::kSkill: {
        // Try localized skill label first if present in carchallenges, else canonical FH1 name.
        const std::string fallback = skill_display_name(p.raw, p.min_grade, p.max_grade);
        if (auto s = lookup_str(strings, "carchallenges", "IDS_Skill_" + p.raw); s && !s->empty()) return *s;
        return fallback;
      }
      case DescParam::Kind::kLocation: {
        if (auto s = lookup_str(strings, "carchallengetransforms", p.raw); s && !s->empty()) return *s;
        if (auto s = lookup_str(strings, "carchallengetransforms", "IDS_" + p.raw); s && !s->empty()) return *s;
        return location_display_name(p.raw);
      }
      case DescParam::Kind::kSpeedCamera: {
        if (auto s = lookup_str(strings, "carchallengetransforms", p.raw); s && !s->empty()) return *s;
        if (auto s = lookup_str(strings, "carchallenges", p.raw); s && !s->empty()) return *s;
        return camera_display_name(p.raw);
      }
      case DescParam::Kind::kEventId: {
        if (event_names) {
          if (auto it = event_names->find(p.event_id); it != event_names->end() && !it->second.empty())
            return it->second;
        }
        return "Event #" + std::to_string(p.event_id);
      }
    }
    return p.raw;
  };

  // Substitute both `{N}` and typed FH1 v4 placeholders like `{0 - Number}`, `{0 - Speed}`, `{1 - Time}`.
  size_t pos = 0;
  while ((pos = tmpl.find('{', pos)) != std::string::npos) {
    const size_t close = tmpl.find('}', pos + 1);
    if (close == std::string::npos) break;
    const std::string inner = tmpl.substr(pos + 1, close - pos - 1);
    if (inner.empty() || inner[0] < '0' || inner[0] > '4') {
      pos = close + 1;
      continue;
    }
    const int idx = inner[0] - '0';
    std::string val = resolve_param(desc_params[size_t(idx)]);
    if (!val.empty()) {
      const std::string spec = lower(inner);
      if (desc_params[size_t(idx)].kind == DescParam::Kind::kNumber) {
        if (has_substr(spec, "speed")) val += " mph";
        else if (has_substr(spec, "dist")) val += " m";
      }
      tmpl.replace(pos, close - pos + 1, val);
      pos += val.size();
    } else {
      pos = close + 1;
    }
  }
  return tmpl;
}

std::string Challenge::format_hint(int index, const fh1::StringTables* strings) const {
  const std::string& id = index == 0 ? hint0 : hint1;
  if (id.empty()) return {};
  if (auto s = lookup_str(strings, "carchallengehints", id); s && !s->empty()) return *s;
  if (auto s = lookup_str(strings, "carchallenges", id); s && !s->empty()) return *s;
  std::string h = id;
  if (h.rfind("IDS_", 0) == 0) h = h.substr(4);
  return title_case_words(h);
}

bool parse_challenges(std::string_view xml, std::vector<Challenge>& out, std::string* error) {
  audio::XmlNode root;
  if (!audio::parse_xml(xml, root, error)) return false;
  if (!audio::iequals(root.name, "car_challenges")) return fail(error, "not a car_challenges file");
  out.clear();
  const auto& kids = root.children;
  for (size_t i = 0; i < kids.size(); ++i) {
    if (!audio::iequals(kids[i].name, "display")) return fail(error, "challenge without a display: " + kids[i].name);
    if (i + 1 >= kids.size()) return fail(error, "display without a challenge");
    Challenge c;
    c.title = kids[i].attr("title");
    c.description = kids[i].attr("description");
    c.hint0 = kids[i].attr("hint0");
    c.hint1 = kids[i].attr("hint1");
    Parser p{&c};
    c.root = p.node(kids[++i]);
    Hasher hasher;
    hasher.node(c.root);
    c.definition_hash = hasher.h;
    out.push_back(std::move(c));
  }
  return true;
}

bool Location::inside(double x, double z) const {
  for (size_t i = 0; i < points.size(); ++i) {
    const double dx = x - points[i][0], dz = z - points[i][2];
    if (dx * dx + dz * dz <= radius[i] * radius[i]) return true;
  }
  return false;
}

bool Location::inside_first(double x, double z) const {
  if (points.empty()) return false;
  const double dx = x - points[0][0], dz = z - points[0][2];
  return dx * dx + dz * dz <= radius[0] * radius[0];
}

bool parse_named_transforms(std::string_view xml, Locations& out, std::string* error) {
  audio::XmlNode root;
  if (!audio::parse_xml(xml, root, error)) return false;
  out.clear();
  for (const auto& t : root.children) {
    if (!audio::iequals(t.name, "NamedTransform")) continue;
    Location& l = out[lower(t.attr("name"))];
    for (const auto& p : t.children) {
      if (!audio::iequals(p.name, "Transform")) continue;
      l.points.push_back({p.number("pos.x"), p.number("pos.y"), p.number("pos.z")});
      l.radius.push_back(p.number("radius", kDefaultRadius));
    }
  }
  if (out.empty()) return fail(error, "no NamedTransform");
  return true;
}

size_t ClubData::challenges() const {
  size_t n = 0;
  for (const auto& [id, list] : cars) n += list.size();
  return n;
}

size_t ClubData::measured() const {
  size_t n = 0;
  for (const auto& [id, list] : cars)
    for (const auto& c : list) n += c.supported;
  return n;
}

bool load_club(const std::string& state_root, ClubData& out, std::string* error) {
  namespace fs = std::filesystem;
  out = {};
  if (state_root.empty()) return fail(error, "no state root");
  // user/<xuid>/4D5309C9/00000002/<package>/Media/DLCZips/<offer>/Media/carchallenges
  const fs::path content = fs::path(state_root) / "user" / "0000000000000000" / "4D5309C9" / "00000002";
  std::error_code ec;
  fs::path dir;
  for (const auto& pkg : fs::directory_iterator(content, ec)) {
    const fs::path zips = pkg.path() / "Media" / "DLCZips";
    for (const auto& offer : fs::directory_iterator(zips, ec)) {
      const fs::path d = offer.path() / "Media" / "carchallenges";
      if (fs::is_directory(d, ec)) dir = d, out.package = pkg.path().filename().string();
    }
  }
  if (dir.empty()) return fail(error, "the 1000 Club package is not installed");
  for (const auto& f : fs::directory_iterator(dir, ec)) {
    const std::string name = lower(f.path().filename().string());
    if (name.rfind("carchallenges_", 0) != 0 || f.path().extension() != ".xml") continue;
    const int64_t id = std::atoll(name.c_str() + 14);
    std::ifstream in(f.path(), std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string why;
    if (!parse_challenges(text, out.cars[id], &why)) return fail(error, name + ": " + why);
  }
  std::string why;
  auto zip = fh1::Archive::open(fs::path(state_root) / "title-update-v4" / "media.zip", &why);
  if (!zip) return fail(error, "title-update-v4/media.zip: " + why);
  auto nt = zip->read("carchallenges/named_transforms.xml");
  if (!nt.ok()) return fail(error, "named_transforms.xml: " + nt.error);
  const std::string_view text(reinterpret_cast<const char*>(nt.data.data()), nt.data.size());
  if (!parse_named_transforms(text, out.locations, &why)) return fail(error, "named_transforms.xml: " + why);
  return true;
}

namespace {

bool british(const CarMeta& m) {
  const std::string mk = lower(m.make);
  const std::string co = lower(m.country);
  const bool british_make =
      has_substr(mk, "aston") || has_substr(mk, "bentley") || has_substr(mk, "jaguar") ||
      has_substr(mk, "land rover") || has_substr(mk, "lotus") || has_substr(mk, "mclaren") ||
      has_substr(mk, "mini") || has_substr(mk, "noble") || has_substr(mk, "tvr") ||
      has_substr(mk, "vauxhall") || has_substr(mk, "radical") || has_substr(mk, "ultima") ||
      has_substr(mk, "austin") || has_substr(mk, "ascari") || has_substr(mk, "bowler") ||
      has_substr(mk, "caterham") || has_substr(mk, "eagle") || has_substr(mk, "joss");
  const bool british_country = (co == "uk" || has_substr(co, "brit") || has_substr(co, "united kingdom") ||
                                has_substr(co, "unitedkingdom") || has_substr(co, "england"));
  return british_make || british_country;
}

}  // namespace

// The package's achievement table (spa 216..225) with the targets above.
const std::array<ClubAchievementDef, 10>& club_achievements() {
  static const std::array<ClubAchievementDef, 10> k = {{
      {216, 60, kClubThresholdFirstMedal, false},
      {217, 61, kClubThresholdFerrari, true},
      {218, 62, kClubThresholdMillionCr, true},
      {219, 63, kClubThresholdShelby, true},
      {220, 64, kClubThresholdLamborghini, true},
      {221, 65, kClubThresholdNissan, true},
      {222, 66, kClubThresholdBmw, true},
      {223, 67, kClubThresholdBritish, true},
      {224, 68, kClubThresholdFord, true},
      {225, 69, kClubThresholdMultiCars, true},
  }};
  return k;
}

const ClubAchievementDef* club_achievement(uint16_t id) {
  for (const auto& a : club_achievements())
    if (a.id == id) return &a;
  return nullptr;
}

bool counts_for(uint16_t id, const CarMeta& m) {
  const std::string mk = lower(m.make);
  switch (id) {
    case 216: case 225: return true;
    case 217: return has_substr(mk, "ferrari");
    case 218: return m.base_cost >= 1000000;
    case 219: return has_substr(mk, "shelby");
    case 220: return has_substr(mk, "lamborghini");
    case 221: return has_substr(mk, "nissan");
    case 222: return has_substr(mk, "bmw");
    case 223: return british(m);
    case 224: return has_substr(mk, "ford");
    default: return false;
  }
}

int achievement_progress(uint16_t id, const ClubMedalStats& st) {
  switch (id) {
    case 216: return st.total_medals;
    case 217: return st.ferrari_cars;
    case 218: return st.million_cr_cars;
    case 219: return st.shelby_cars;
    case 220: return st.lamborghini_cars;
    case 221: return st.nissan_cars;
    case 222: return st.bmw_cars;
    case 223: return st.british_cars;
    case 224: return st.ford_cars;
    case 225: return st.cars_with_medal;
    default: return 0;
  }
}

CarMedals car_medals(const ClubData& data, int64_t car_id, const std::vector<bool>* completed) {
  CarMedals m;
  if (auto it = data.cars.find(car_id); it != data.cars.end()) m.available = int(it->second.size());
  if (completed) {
    m.begun = true;
    for (bool c : *completed) m.earned += c ? 1 : 0;
    m.earned = std::min(m.earned, m.available);
  }
  return m;
}

ClubMedalStats compute_medal_stats(const std::vector<std::pair<int64_t, int>>& medals_per_car,
                                   const std::map<int64_t, CarMeta>& meta) {
  ClubMedalStats st;
  std::set<int64_t> seen_cars;
  for (const auto& [car_id, medals] : medals_per_car) {
    if (medals <= 0) continue;
    st.total_medals += medals;
    if (!seen_cars.insert(car_id).second) continue;
    ++st.cars_with_medal;
    auto it = meta.find(car_id);
    if (it == meta.end()) continue;
    const CarMeta& m = it->second;
    st.ford_cars += counts_for(224, m);
    st.ferrari_cars += counts_for(217, m);
    st.shelby_cars += counts_for(219, m);
    st.lamborghini_cars += counts_for(220, m);
    st.nissan_cars += counts_for(221, m);
    st.bmw_cars += counts_for(222, m);
    st.british_cars += counts_for(223, m);
    st.million_cr_cars += counts_for(218, m);
  }
  st.ach_first_medal = st.total_medals >= kClubThresholdFirstMedal;
  st.ach_multi_cars = st.cars_with_medal >= kClubThresholdMultiCars;
  st.ach_ford = st.ford_cars >= kClubThresholdFord;
  st.ach_ferrari = st.ferrari_cars >= kClubThresholdFerrari;
  st.ach_shelby = st.shelby_cars >= kClubThresholdShelby;
  st.ach_lamborghini = st.lamborghini_cars >= kClubThresholdLamborghini;
  st.ach_nissan = st.nissan_cars >= kClubThresholdNissan;
  st.ach_bmw = st.bmw_cars >= kClubThresholdBmw;
  st.ach_british = st.british_cars >= kClubThresholdBritish;
  st.ach_million_cr = st.million_cr_cars >= kClubThresholdMillionCr;
  st.achievements_unlocked = int(st.ach_first_medal) + int(st.ach_multi_cars) + int(st.ach_ford) +
                             int(st.ach_ferrari) + int(st.ach_shelby) + int(st.ach_lamborghini) +
                             int(st.ach_nissan) + int(st.ach_bmw) + int(st.ach_british) + int(st.ach_million_cr);
  return st;
}

Run::Run(const Challenge& challenge, const Locations& locations) : c_(&challenge), locs_(&locations) {
  std::string missing;
  check_locations(c_->root, *locs_, missing);
  unsupported_ = !c_->supported || !missing.empty();
  const bool explicit_bank =
      has_substr(lower(c_->title), "bankskillpoints") || has_substr(lower(c_->description), "bankskillpoints");
  single_chain_skill_points_ =
      !explicit_bank && (tree_has_chain_fail(c_->root) || has_substr(lower(c_->title), "samechain") ||
                         has_substr(lower(c_->description), "samechain"));
  build(c_->root, root_);
}

bool Run::is_cumulative_skill_points(const Node& n) const {
  if (!find_skill_points_cond(n)) return false;
  return n.save_progress || !single_chain_skill_points_;
}

bool Run::node_saves_progress(const Node& n) {
  if (n.save_progress) return true;
  if (n.kind == Kind::kOr && n.pass_count > 1) {
    for (const auto& ch : n.children)
      if (!is_fail(ch) && has_saved_rec(ch)) return true;
  }
  return false;
}

void Run::build(const Node& n, State& st) {
  st.children.resize(n.children.size());
  for (size_t i = 0; i < n.children.size(); ++i) build(n.children[i], st.children[i]);
}

void Run::activate(const Node& n, State& st, double t, bool keep_saved) {
  const bool preserve = keep_saved && node_saves_progress(n);
  st.active = true, st.start = t, st.current = 0, st.chains = 0, st.was_true = false, st.started = false;
  st.times.clear();
  if (!preserve) {
    st.done = false;
    st.count = 0;
    st.accum_value = 0.0;
    st.seen_cameras.clear();
    st.camera_hits.clear();
  }
  if (n.kind == Kind::kAToB) return;  // its children start with the trip
  for (size_t i = 0; i < n.children.size(); ++i) activate(n.children[i], st.children[i], t, keep_saved);
}

bool Run::holds(const Condition& c, const Node& owner_node, const State& owner, const Signal& s) const {
  switch (c.kind) {
    case Cond::kSpeed: return compare(s.speed_mph, c.cmp, c.value);
    case Cond::kTimer: return compare(s.time - owner.start, c.cmp, c.value);
    case Cond::kGear: return compare(double(s.gear), c.cmp, c.value);
    case Cond::kDistance: return compare(s.jump_m, c.cmp, c.value);
    case Cond::kInAir: return s.in_air;
    case Cond::kIsDrifting: return s.is_drifting;
    case Cond::kCollided: return s.collided;
    case Cond::kLocation: {
      auto it = locs_->find(c.name);
      return it != locs_->end() && it->second.inside(s.x, s.z) == c.inside;
    }
    case Cond::kSkills: {
      for (const auto& a : s.skills_awarded) {
        if (lower(a.type) == c.name && a.grade <= c.min_grade && a.grade >= c.max_grade) return true;
      }
      return false;
    }
    case Cond::kSkillPoints: {
      const double pts = is_cumulative_skill_points(owner_node) ? owner.accum_value : s.skill_points;
      return compare(pts, c.cmp, c.value);
    }
    case Cond::kNumSkillChains: return compare(double(owner.chains), c.cmp, c.value);
    case Cond::kSmashables: {
      for (const auto& sm : s.smashables_broken) {
        if (c.name.empty() || lower(sm) == c.name) return true;
      }
      return false;
    }
    case Cond::kSpeedCamera: {
      for (const auto& h : s.speed_cameras) {
        if (camera_matches_target(c, h) && camera_matches_speed(c, h)) return true;
      }
      return false;
    }
    case Cond::kEventStarted: return s.event_started;
    case Cond::kEventRestarted: return s.event_restarted;
    case Cond::kEventCompleted:
      return s.event_completed && s.event_finish_place >= 1 && s.event_finish_place <= c.min_finish_place;
    case Cond::kEventId: return compare(double(s.event_id), c.cmp, c.value);
    case Cond::kEventHub: return !s.event_hub.empty() && lower(s.event_hub) == c.name;
    case Cond::kCareerEventStyle: {
      const std::string cur = lower(s.career_event_style);
      if (c.name == "none") return cur.empty() || cur == "none";
      return cur == c.name;
    }
    case Cond::kEncounterStarted: return s.encounter_started;
    case Cond::kEncounterCompleted:
      return s.encounter_completed && s.encounter_finish_place >= 1 && s.encounter_finish_place <= c.min_finish_place;
    case Cond::kRaceProgress: return compare(s.race_progress, c.cmp, c.value);
    case Cond::kUnsupported: return false;
  }
  return false;
}

bool Run::progressed(const Node& n, const State& st) const {
  if (st.done || st.count > 0 || st.current > 0 || st.started || st.accum_value > 0.0 || !st.camera_hits.empty() ||
      !st.seen_cameras.empty())
    return true;
  for (size_t i = 0; i < n.children.size(); ++i)
    if (!is_fail(n.children[i]) && progressed(n.children[i], st.children[i])) return true;
  return false;
}

Run::Result Run::advance(const Node& n, State& st, const Signal& s) {
  if (st.done) return Result::kPassed;
  if (s.skill_chain_completed) ++st.chains;

  auto all = [&] {
    for (const auto& c : n.conditions)
      if (!holds(c, n, st, s)) return false;
    return !n.conditions.empty();
  };
  auto all_state_conds = [&] {
    for (const auto& c : n.conditions)
      if (!is_event_cond(c.kind) && !holds(c, n, st, s)) return false;
    return true;
  };
  auto all_non_skill_points_conds = [&] {
    for (const auto& c : n.conditions)
      if (c.kind != Cond::kSkillPoints && !holds(c, n, st, s)) return false;
    return true;
  };
  // A group's pass: once, or again until its pass_count.
  auto group_passed = [&] {
    if (++st.count >= n.pass_count) {
      st.done = true;
      return Result::kPassed;
    }
    for (size_t i = 0; i < n.children.size(); ++i) activate(n.children[i], st.children[i], s.time, false);
    st.current = 0;
    return Result::kRunning;
  };

  switch (n.kind) {
    case Kind::kFail: return all() ? Result::kFailed : Result::kRunning;
    case Kind::kFailUnless: return all() ? Result::kRunning : Result::kFailed;
    case Kind::kPass: {
      // Special handling for speed_camera (combined-speed sum, unique traps, timed intervals).
      const Condition* cam = nullptr;
      bool has_event = false;
      for (const auto& c : n.conditions) {
        if (c.kind == Cond::kSpeedCamera) cam = &c;
        if (is_event_cond(c.kind)) has_event = true;
      }
      if (cam && all_state_conds()) {
        const bool combined_mode = (n.pass_count > 1 && n.pass_interval <= 0 && cam->min_speed > 0);
        if (combined_mode) {
          for (const auto& h : s.speed_cameras) {
            if (!camera_matches_target(*cam, h)) continue;
            const std::string key = canonical_camera_key(h);
            if (cam->unique) {
              bool updated = false;
              for (auto& prev : st.camera_hits) {
                if (prev.key == key) {
                  prev.speed_mph = std::max(prev.speed_mph, h.speed_mph);
                  prev.time = s.time;
                  updated = true;
                  break;
                }
              }
              if (!updated) {
                st.camera_hits.push_back({s.time, key, h.speed_mph});
                if (int(st.camera_hits.size()) > n.pass_count) {
                  auto lowest = std::min_element(
                      st.camera_hits.begin(), st.camera_hits.end(),
                      [](const CameraSample& a, const CameraSample& b) { return a.speed_mph < b.speed_mph; });
                  st.camera_hits.erase(lowest);
                }
              }
            } else {
              st.camera_hits.push_back({s.time, key, h.speed_mph});
              while (int(st.camera_hits.size()) > n.pass_count) st.camera_hits.pop_front();
            }
          }
          st.count = int(st.camera_hits.size());
          double sum_mph = 0;
          for (const auto& hit : st.camera_hits) sum_mph += hit.speed_mph;
          if (st.count >= n.pass_count && sum_mph >= cam->min_speed) st.done = true;
          return st.done ? Result::kPassed : Result::kRunning;
        }
        if (n.pass_interval > 0) {
          for (const auto& h : s.speed_cameras) {
            if (!camera_matches_target(*cam, h) || !camera_matches_speed(*cam, h)) continue;
            st.camera_hits.push_back({s.time, canonical_camera_key(h), h.speed_mph});
          }
          while (!st.camera_hits.empty() && st.camera_hits.front().time < s.time - n.pass_interval)
            st.camera_hits.pop_front();
          if (cam->unique) {
            std::set<std::string> uniq;
            for (const auto& hit : st.camera_hits) uniq.insert(hit.key);
            st.count = int(uniq.size());
          } else {
            st.count = int(st.camera_hits.size());
          }
          if (st.count >= n.pass_count) st.done = true;
          return st.done ? Result::kPassed : Result::kRunning;
        }
        // Standard single or repeated speed_camera pass_when.
        for (const auto& h : s.speed_cameras) {
          if (!camera_matches_target(*cam, h) || !camera_matches_speed(*cam, h)) continue;
          const std::string key = canonical_camera_key(h);
          if (cam->unique && !st.seen_cameras.insert(key).second) continue;
          if (++st.count >= n.pass_count) {
            st.done = true;
            break;
          }
        }
        return st.done ? Result::kPassed : Result::kRunning;
      }

      // Event-pulse conditions (skills, smashables, event_*, encounter_*): count every matching occurrence
      // on this step without requiring a false gap between consecutive steps.
      if (has_event && !cam) {
        if (!all_state_conds()) {
          st.was_true = false;
          return Result::kRunning;
        }
        int occurrences = 0;
        bool all_events_matched = true;
        for (const auto& c : n.conditions) {
          if (!is_event_cond(c.kind)) continue;
          int matches = 0;
          if (c.kind == Cond::kSkills) {
            for (const auto& a : s.skills_awarded)
              if (lower(a.type) == c.name && a.grade <= c.min_grade && a.grade >= c.max_grade) ++matches;
          } else if (c.kind == Cond::kSmashables) {
            for (const auto& sm : s.smashables_broken)
              if (c.name.empty() || lower(sm) == c.name) ++matches;
          } else if (holds(c, n, st, s)) {
            matches = 1;
          }
          if (matches == 0) {
            all_events_matched = false;
            break;
          }
          occurrences = (occurrences == 0) ? matches : std::min(occurrences, matches);
        }
        if (all_events_matched && occurrences > 0) {
          for (int k = 0; k < occurrences; ++k) {
            if (n.pass_interval > 0) {
              st.times.push_back(s.time);
              while (!st.times.empty() && st.times.front() < s.time - n.pass_interval) st.times.pop_front();
              st.count = int(st.times.size());
              if (st.count >= n.pass_count) {
                st.done = true;
                break;
              }
            } else {
              if (++st.count >= n.pass_count) {
                st.done = true;
                break;
              }
            }
          }
        } else if (n.pass_interval > 0) {
          while (!st.times.empty() && st.times.front() < s.time - n.pass_interval) st.times.pop_front();
          st.count = int(st.times.size());
        }
        return st.done ? Result::kPassed : Result::kRunning;
      }

      // Cumulative banked skill_points accumulation (no num_skill_chains fail / save_progress).
      if (is_cumulative_skill_points(n) && s.banked_skill_points_delta > 0.0 && all_non_skill_points_conds()) {
        st.accum_value += s.banked_skill_points_delta;
      }

      // State-only conditions (original edge-triggered semantics).
      const bool h = all(), edge = h && !st.was_true;
      st.was_true = h;
      if (n.pass_count <= 1 && n.pass_interval <= 0) {
        if (h) st.done = true;
      } else if (edge) {
        if (n.pass_interval > 0) {
          st.times.push_back(s.time);
          while (!st.times.empty() && st.times.front() < s.time - n.pass_interval) st.times.pop_front();
          st.count = int(st.times.size());
          if (st.count >= n.pass_count) st.done = true;
        } else if (++st.count >= n.pass_count) {
          st.done = true;
        }
      }
      return st.done ? Result::kPassed : Result::kRunning;
    }
    case Kind::kAnd:
    case Kind::kOr: {
      bool every = true, any = false;
      for (size_t i = 0; i < n.children.size(); ++i) {
        const Result r = advance(n.children[i], st.children[i], s);
        if (r == Result::kFailed) return Result::kFailed;
        if (is_fail(n.children[i])) continue;
        every = every && r == Result::kPassed;
        any = any || r == Result::kPassed;
      }
      return (n.kind == Kind::kAnd ? every : any) ? group_passed() : Result::kRunning;
    }
    case Kind::kThen: {
      for (size_t i = 0; i < n.children.size(); ++i)
        if (is_fail(n.children[i]) && advance(n.children[i], st.children[i], s) == Result::kFailed)
          return Result::kFailed;
      for (;;) {
        size_t step = 0, k = 0;
        for (; k < n.children.size(); ++k)
          if (!is_fail(n.children[k]) && int(step++) == st.current) break;
        if (k == n.children.size()) return group_passed();
        const Result r = advance(n.children[k], st.children[k], s);
        if (r == Result::kFailed) return Result::kFailed;
        if (r != Result::kPassed) return Result::kRunning;
        ++st.current;
        for (size_t j = k + 1; j < n.children.size(); ++j)
          if (!is_fail(n.children[j])) {
            activate(n.children[j], st.children[j], s.time, true);
            break;
          }
      }
    }
    case Kind::kAToB: {
      const Location& a = locs_->at(n.start_loc);
      const Location& b = locs_->at(n.end_loc);
      if (!st.started) {
        const bool in_a = a.inside(s.x, s.z);
        if (in_a || !st.was_true) {
          st.was_true = in_a;
          return Result::kRunning;
        }
        st.started = true;
        st.start = s.time;
        for (size_t i = 0; i < n.children.size(); ++i) activate(n.children[i], st.children[i], s.time, true);
      }
      bool every = true;
      for (size_t i = 0; i < n.children.size(); ++i) {
        const Result r = advance(n.children[i], st.children[i], s);
        if (r == Result::kFailed) return Result::kFailed;
        if (!is_fail(n.children[i])) every = every && r == Result::kPassed;
      }
      if (every && b.inside(s.x, s.z)) {
        st.done = true;
        return Result::kPassed;
      }
      return Result::kRunning;
    }
  }
  return Result::kRunning;
}

bool Run::step(const Signal& s) {
  if (passed_ || unsupported_) return false;
  if (!root_.active) activate(c_->root, root_, s.time, true);
  const Result r = advance(c_->root, root_, s);
  if (r == Result::kPassed) return passed_ = true;
  if (r == Result::kFailed) {
    if (progressed(c_->root, root_)) ++failures_;
    activate(c_->root, root_, s.time, true);
  }
  return false;
}

bool Run::under_way(const Signal& s) const {
  if (passed_ || unsupported_ || !root_.active) return false;
  if (progressed(c_->root, root_)) return true;
  // A held timer/speed condition (e.g. hold >= 90 mph for 10 s); passive waits
  // for a trip, skill, event, or camera are not "under way" until progressed.
  std::vector<const Node*> stack{&c_->root};
  bool has_held_timer = false;
  while (!stack.empty()) {
    const Node* n = stack.back();
    stack.pop_back();
    if (n->kind == Kind::kAToB) return false;
    if (n->kind == Kind::kPass) {
      for (const auto& c : n->conditions)
        if (c.kind == Cond::kTimer) has_held_timer = true;
    }
    for (const auto& k : n->children) stack.push_back(&k);
  }
  return has_held_timer && s.time - root_.start > 1.0;
}

double Run::progress(const Signal& s) const {
  if (passed_) return 1.0;
  if (unsupported_ || !root_.active) return 0.0;
  // Check for counted pass_when / group progress first.
  std::vector<std::pair<const Node*, const State*>> stack{{&c_->root, &root_}};
  while (!stack.empty()) {
    auto [n, st] = stack.back();
    stack.pop_back();
    if (n->pass_count > 1 && st->count > 0) {
      return std::clamp(double(st->count) / double(n->pass_count), 0.0, 1.0);
    }
    if (n->kind == Kind::kPass) {
      for (const auto& c : n->conditions) {
        if (c.kind == Cond::kSkillPoints && c.value > 0) {
          const double pts = is_cumulative_skill_points(*n) ? st->accum_value : s.skill_points;
          if (pts > 0.0) return std::clamp(pts / c.value, 0.0, 1.0);
        }
        if (c.kind == Cond::kTimer && c.value > 0 && st->active)
          return std::clamp((s.time - st->start) / c.value, 0.0, 1.0);
      }
    }
    for (size_t i = 0; i < n->children.size(); ++i)
      if (!is_fail(n->children[i])) stack.push_back({&n->children[i], &st->children[i]});
  }
  if (c_->root.kind == Kind::kThen) {
    int steps = 0;
    for (const auto& k : c_->root.children) steps += !is_fail(k);
    if (steps > 0) return std::clamp(double(root_.current) / double(steps), 0.0, 1.0);
  }
  return 0.0;
}

std::string Run::status(const Signal& s) const {
  if (passed_) return "done";
  if (unsupported_) {
    std::string missing = c_->unsupported;
    if (missing.empty()) check_locations(c_->root, *locs_, missing);
    return "not measured yet (" + missing + ")";
  }
  char b[160];
  std::vector<std::pair<const Node*, const State*>> stack{{&c_->root, &root_}};
  while (!stack.empty()) {
    auto [n, st] = stack.back();
    stack.pop_back();
    if (n->kind == Kind::kAToB) {
      if (!st->started)
        std::snprintf(b, sizeof b, st->was_true ? "leave %s to start" : "drive into %s", n->start_loc.c_str());
      else
        std::snprintf(b, sizeof b, "%.0f s on the way to %s", s.time - st->start, n->end_loc.c_str());
      return b;
    }
    if (n->pass_count > 1 && (n->kind == Kind::kPass || n->kind == Kind::kOr || n->kind == Kind::kAnd)) {
      // Combined speed camera progress if active.
      if (!st->camera_hits.empty() && n->pass_interval <= 0) {
        double sum = 0;
        for (const auto& h : st->camera_hits) sum += h.speed_mph;
        std::snprintf(b, sizeof b, "%d/%d (%.0f mph)", st->count, n->pass_count, sum);
        return b;
      }
      std::snprintf(b, sizeof b, "%d / %d", st->count, n->pass_count);
      return b;
    }
    for (size_t i = 0; i < n->children.size(); ++i)
      if (!is_fail(n->children[i])) stack.push_back({&n->children[i], &st->children[i]});
  }
  if (c_->root.kind == Kind::kThen) {
    int steps = 0;
    for (const auto& k : c_->root.children) steps += !is_fail(k);
    std::snprintf(b, sizeof b, "step %d of %d, %.1f s", root_.current + 1, steps, s.time - root_.start);
  } else {
    std::snprintf(b, sizeof b, "%.1f s", s.time - root_.start);
  }
  return b;
}

void Run::export_saved_rec(const Node& n, const State& st, std::vector<int>& out) const {
  if (node_saves_progress(n)) {
    if (is_cumulative_skill_points(n)) {
      out.push_back(std::max(0, int(std::llround(st.accum_value))));
    } else if (const Condition* cam = find_speed_camera_cond(n)) {
      const bool combined_mode = (n.pass_count > 1 && n.pass_interval <= 0 && cam->min_speed > 0);
      out.push_back(std::clamp(st.count, 0, n.pass_count));
      if (combined_mode) {
        out.push_back(int(st.camera_hits.size()));
        for (const auto& hit : st.camera_hits) {
          out.push_back(encode_camera_key(hit.key));
          out.push_back(std::max(0, int(std::llround(hit.speed_mph * 10.0))));
        }
      } else if (cam->unique) {
        out.push_back(int(st.seen_cameras.size()));
        for (const auto& key : st.seen_cameras) out.push_back(encode_camera_key(key));
      }
    } else {
      out.push_back(std::clamp(st.count, 0, n.pass_count));
    }
  }
  for (size_t i = 0; i < n.children.size() && i < st.children.size(); ++i)
    export_saved_rec(n.children[i], st.children[i], out);
}

void Run::import_saved_rec(const Node& n, State& st, const std::vector<int>& in, size_t& idx) {
  if (node_saves_progress(n)) {
    if (is_cumulative_skill_points(n)) {
      if (idx < in.size()) {
        st.accum_value = double(std::max(0, in[idx]));
        if (const Condition* sp = find_skill_points_cond(n)) {
          if (compare(st.accum_value, sp->cmp, sp->value)) {
            st.done = true;
            st.count = n.pass_count;
          }
        }
      }
      ++idx;
    } else if (const Condition* cam = find_speed_camera_cond(n)) {
      const bool combined_mode = (n.pass_count > 1 && n.pass_interval <= 0 && cam->min_speed > 0);
      if (idx < in.size()) {
        st.count = std::clamp(in[idx], 0, n.pass_count);
      }
      ++idx;
      if (combined_mode) {
        if (idx < in.size()) {
          const int n_hits = std::clamp(in[idx++], 0, n.pass_count);
          st.camera_hits.clear();
          st.seen_cameras.clear();
          double sum_mph = 0.0;
          for (int k = 0; k < n_hits && idx + 1 < in.size(); ++k) {
            const std::string key = decode_camera_key(in[idx++]);
            const double spd = double(std::max(0, in[idx++])) / 10.0;
            if (!key.empty()) {
              st.camera_hits.push_back({0.0, key, spd});
              if (cam->unique) st.seen_cameras.insert(key);
              sum_mph += spd;
            }
          }
          if (!st.camera_hits.empty()) st.count = std::clamp(int(st.camera_hits.size()), 0, n.pass_count);
          if (st.count >= n.pass_count && sum_mph >= cam->min_speed) st.done = true;
        }
      } else if (cam->unique) {
        if (idx < in.size()) {
          const int n_seen = std::clamp(in[idx++], 0, 256);
          st.seen_cameras.clear();
          for (int k = 0; k < n_seen && idx < in.size(); ++k) {
            const std::string key = decode_camera_key(in[idx++]);
            if (!key.empty()) st.seen_cameras.insert(key);
          }
          st.count = std::clamp(std::max(st.count, int(st.seen_cameras.size())), 0, n.pass_count);
        }
        if (st.count >= n.pass_count) st.done = true;
      } else {
        if (st.count >= n.pass_count) st.done = true;
      }
    } else {
      if (idx < in.size()) {
        st.count = std::clamp(in[idx], 0, n.pass_count);
        if (st.count >= n.pass_count) st.done = true;
      }
      ++idx;
    }
  }
  for (size_t i = 0; i < n.children.size() && i < st.children.size(); ++i)
    import_saved_rec(n.children[i], st.children[i], in, idx);
}

bool Run::has_saved_rec(const Node& n) {
  if (n.save_progress) return true;
  for (const auto& ch : n.children)
    if (has_saved_rec(ch)) return true;
  return false;
}

std::vector<int> Run::export_save_progress() const {
  std::vector<int> out;
  export_saved_rec(c_->root, root_, out);
  return out;
}

void Run::import_save_progress(const std::vector<int>& counts) {
  size_t idx = 0;
  import_saved_rec(c_->root, root_, counts, idx);
}

bool Run::has_save_progress() const { return has_saved_rec(c_->root); }

}  // namespace freeroam::club
