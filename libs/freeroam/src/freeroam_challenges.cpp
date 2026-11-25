#include "freeroam/freeroam_challenges.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

#include "audio/xml.h"
#include "fh1data/archive.h"
#include "fh1data/stringtables.h"

namespace freeroam {
namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

bool fail(std::string* error, const std::string& what) {
  if (error) *error = what;
  return false;
}

std::string lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool truthy(std::string_view v) { return audio::iequals(v, "true") || v == "1"; }

FreeRoamChallengeType type_of(std::string_view t) {
  if (audio::iequals(t, "DistanceDrivenFreeRoamChallenge")) return FreeRoamChallengeType::kDistanceDriven;
  if (audio::iequals(t, "DriveFromAToBFreeRoamChallenge")) return FreeRoamChallengeType::kDriveFromAToB;
  if (audio::iequals(t, "SkillsFreeRoamChallenge")) return FreeRoamChallengeType::kSkills;
  if (audio::iequals(t, "SpeedCameraFreeRoamChallenge")) return FreeRoamChallengeType::kSpeedCamera;
  return FreeRoamChallengeType::kUnknown;
}

// CarRestriction attribute -> field and which bound it sets (0 exact, 1 min, 2 max).
struct CarAttr {
  const char* name;
  CarField field;
  int bound;
};
constexpr CarAttr kCarAttrs[] = {
    {"CarClassId", CarField::kClass, 0},         {"PIMin", CarField::kPi, 1},
    {"PIMax", CarField::kPi, 2},                 {"PowerMin", CarField::kPower, 1},
    {"PowerMax", CarField::kPower, 2},           {"WeightMin", CarField::kWeight, 1},
    {"WeightMax", CarField::kWeight, 2},         {"YearMin", CarField::kYear, 1},
    {"YearMax", CarField::kYear, 2},             {"BodyFamilyId", CarField::kBodyFamily, 0},
    {"CarTypeId", CarField::kCarType, 0},        {"RegionId", CarField::kRegion, 0},
    {"CountryId", CarField::kCountry, 0},        {"MakeId", CarField::kMake, 0},
    {"ModelFamilyId", CarField::kModelFamily, 0}, {"ModelId", CarField::kModel, 0},
    {"DriveTypeId", CarField::kDriveType, 0},    {"EngineConfigId", CarField::kEngineConfig, 0},
    {"EnginePlacementId", CarField::kEnginePlacement, 0}, {"CylinderId", CarField::kCylinder, 0},
    {"AspirationId", CarField::kAspiration, 0},  {"FamilySpecialId", CarField::kFamilySpecial, 0},
    {"BaseCostMin", CarField::kBaseCost, 1},     {"BaseCostMax", CarField::kBaseCost, 2},
};

ChallengeRestriction parse_restriction(const audio::XmlNode& r) {
  ChallengeRestriction out;
  out.type = r.attr("type");
  if (audio::iequals(out.type, "TimeOfDayRestriction")) {
    out.kind = ChallengeRestriction::Kind::kTimeOfDay;
    out.start_hour = r.number("start_hour", 0) + r.number("start_minute", 0) / 60.0;
    out.end_hour = r.number("end_hour", 0) + r.number("end_minute", 0) / 60.0;
  } else if (audio::iequals(out.type, "SpeedRestriction")) {
    out.kind = ChallengeRestriction::Kind::kSpeed;
    out.min_mph = r.number("min_speed", 0);
    out.max_mph = r.number("max_speed", 0);
  } else if (audio::iequals(out.type, "CarRestriction")) {
    out.kind = ChallengeRestriction::Kind::kCar;
    for (const auto& a : kCarAttrs) {
      if (r.attr(a.name).empty()) continue;  // the XML's blank template attributes
      const double v = r.number(a.name, 0);
      auto it = std::find_if(out.car.begin(), out.car.end(),
                             [&](const ChallengeRestriction::Range& g) { return g.field == a.field; });
      if (it == out.car.end()) {
        out.car.push_back({a.field, -kInf, kInf});
        it = out.car.end() - 1;
      }
      if (a.bound != 2) it->min = v;
      if (a.bound != 1) it->max = v;
    }
    if (const std::string u = r.attr("AllowUpgrades"); !u.empty()) out.allow_upgrades = truthy(u) ? 1 : 0;
  }
  return out;
}

std::string resolve(const fh1::StringTable* strings, const std::string& key) {
  if (!strings || key.empty()) return {};
  const std::u16string* t = strings->find(fh1::string_hash(key));
  return t ? fh1::utf16_to_utf8(*t) : std::string{};
}

void parse_challenges(const audio::XmlNode& root, bool retired, const fh1::StringTable* strings,
                      std::vector<FreeRoamChallengeDef>& out) {
  for (const auto& ch : root.children) {
    if (!audio::iequals(ch.name, "Challenge")) continue;
    FreeRoamChallengeDef d;
    d.id = ch.attr("id");
    if (d.id.empty()) continue;
    d.type_name = ch.attr("type");
    d.type = type_of(d.type_name);
    d.retired = retired;
    d.sponsor_name_key = ch.attr("sponsorName");
    d.challenge_name_key = ch.attr("challengeName");
    d.description_key = ch.attr("challengeDesc");
    d.sponsor_logo = ch.attr("sponsorLogo");
    d.sponsor_name = resolve(strings, d.sponsor_name_key);
    d.challenge_name = resolve(strings, d.challenge_name_key);
    d.description = resolve(strings, d.description_key);
    if (const auto* rw = ch.child("Reward")) d.reward_credits = int64_t(std::llround(rw->number("credits", 0)));
    if (const auto* o = ch.child("Options")) {
      d.distance_to_drive_m = o->number("distance_to_drive", 0);
      d.feat_id = o->attr("feat_id");
      d.target_feat_count = int(o->number("target_feat_count", 0));
      d.start_location = {o->number("start_location.x", 0), o->number("start_location.y", 0),
                          o->number("start_location.z", 0)};
      d.end_location = {o->number("end_location.x", 0), o->number("end_location.y", 0),
                        o->number("end_location.z", 0)};
      d.start_radius = o->number("start_radius", 0);
      d.end_radius = o->number("end_radius", 0);
      d.min_players = int(o->number("min_players", 1));
      d.bidirectional = truthy(o->attr("bidirectional"));
      d.time_limit_s = o->number("time_limit", 0);
      d.num_cameras_to_flash = int(o->number("num_cameras_to_flash", 0));
      d.cameras_must_be_unique = truthy(o->attr("cameras_must_be_unique"));
      for (const auto& c : o->children)
        if (audio::iequals(c.name, "Camera") && !c.attr("camera_id").empty()) d.camera_ids.push_back(c.attr("camera_id"));
    }
    if (const auto* rs = ch.child("Restrictions"))
      for (const auto& r : rs->children)
        if (audio::iequals(r.name, "Restriction")) d.restrictions.push_back(parse_restriction(r));
    out.push_back(std::move(d));
  }
}

}  // namespace

bool ChallengeRestriction::holds(double tod_hour, double speed_mph, const ChallengeCar& c) const {
  switch (kind) {
    case Kind::kTimeOfDay:
      // [start, end), the window wrapping past midnight when end < start.
      if (start_hour <= end_hour) return tod_hour >= start_hour && tod_hour < end_hour;
      return tod_hour >= start_hour || tod_hour < end_hour;
    case Kind::kSpeed:
      if (min_mph > 0 && speed_mph < min_mph) return false;
      if (max_mph > 0 && speed_mph >= max_mph) return false;
      return true;
    case Kind::kCar:
      for (const auto& g : car) {
        const double v = c[g.field];
        if (v < 0 || v < g.min || v > g.max) return false;
      }
      if (allow_upgrades == 0 && c.upgraded) return false;
      return true;
    case Kind::kUnknown:
      break;
  }
  return true;  // a type the engine does not know does not block
}

std::string FreeRoamChallengeDef::logo_texture() const {
  if (sponsor_logo.empty()) return {};
  std::string p = sponsor_logo;
  std::replace(p.begin(), p.end(), '\\', '/');
  const size_t slash = p.rfind('/');
  std::string file = p.substr(slash == std::string::npos ? 0 : slash + 1);
  if (const size_t dot = file.rfind('.'); dot != std::string::npos) file.resize(dot);
  for (char& ch : file) ch = char(std::toupper(static_cast<unsigned char>(ch)));
  return "SPONSORLOGOTILE/" + file;
}

bool FreeRoamChallengeDef::restrictions_hold(double tod_hour, double speed_mph, const ChallengeCar& car) const {
  for (const auto& r : restrictions)
    if (!r.holds(tod_hour, speed_mph, car)) return false;
  return true;
}

const FreeRoamChallengeDef* FreeRoamChallengesData::find(std::string_view id) const {
  for (const auto& c : challenges)
    if (audio::iequals(c.id, id)) return &c;
  return nullptr;
}

size_t FreeRoamChallengesData::live_count() const {
  return size_t(std::count_if(challenges.begin(), challenges.end(), [](const auto& c) { return !c.retired; }));
}

bool parse_free_roam_challenges(std::string_view xml, FreeRoamChallengesData& out, std::string* error,
                                const fh1::StringTable* strings) {
  out = FreeRoamChallengesData{};
  audio::XmlNode root;
  std::string why;
  if (!audio::parse_xml(xml, root, &why)) return fail(error, "FreeRoamChallenges.xml: " + why);
  if (!audio::iequals(root.name, "FreeRoamChallenges"))
    return fail(error, "FreeRoamChallenges.xml: root is <" + root.name + ">");
  parse_challenges(root, false, strings, out.challenges);
  // The retired challenges: comment bodies holding whole <Challenge> elements.
  // (The file's other comments are notes and templates, not XML.)
  for (size_t at = xml.find("<!--"); at != std::string_view::npos; at = xml.find("<!--", at)) {
    const size_t end = xml.find("-->", at + 4);
    if (end == std::string_view::npos) break;
    const std::string_view body = xml.substr(at + 4, end - at - 4);
    at = end + 3;
    if (body.find("<Challenge ") == std::string_view::npos) continue;
    audio::XmlNode old;
    if (!audio::parse_xml("<Retired>" + std::string(body) + "</Retired>", old, nullptr)) continue;
    parse_challenges(old, true, strings, out.challenges);
  }
  if (out.challenges.empty()) return fail(error, "FreeRoamChallenges.xml: no challenges");
  return true;
}

bool load_free_roam_challenges(const std::filesystem::path& game_root, FreeRoamChallengesData& out,
                               std::string* error) {
  std::string why;
  auto zip = fh1::Archive::open(game_root / "media" / "gametunablesettings.zip", &why);
  if (!zip) return fail(error, why);
  auto xml = zip->read("FreeRoamChallenges.xml");
  if (!xml.ok()) return fail(error, "FreeRoamChallenges.xml: " + xml.error);
  fh1::StringTable strings;
  bool have_strings = false;
  if (auto en = fh1::Archive::open(game_root / "media" / "stringtables" / "EN.zip", nullptr)) {
    auto str = en->read("FreeRoamChallenges.str");
    have_strings = str.ok() && fh1::parse_string_table(str.data, strings, nullptr);
  }
  const std::string_view text(reinterpret_cast<const char*>(xml.data.data()), xml.data.size());
  return parse_free_roam_challenges(text, out, error, have_strings ? &strings : nullptr);
}

FreeRoamChallengeTracker::FreeRoamChallengeTracker(const FreeRoamChallengesData& data) : data_(data) {}

bool FreeRoamChallengeTracker::tracked(const FreeRoamChallengeDef& d) const {
  if (d.retired || d.type == FreeRoamChallengeType::kUnknown) return false;
  return active_.empty() || audio::iequals(active_, d.id);
}

std::vector<ChallengeCompletion> FreeRoamChallengeTracker::step(const ChallengeStepInput& in) {
  std::vector<ChallengeCompletion> done;
  for (const auto& d : data_.challenges) {
    if (!tracked(d)) continue;
    State& s = states_[d.id];
    if (s.completed) continue;
    const bool holds = d.restrictions_hold(in.tod_hour, in.speed_mph, in.car);
    bool complete = false;
    switch (d.type) {
      case FreeRoamChallengeType::kDistanceDriven:
        // interim: the original rounds the target to whole display units
        // (miles or km); 12874752 m is exactly 8000 mi, so it is not done here.
        if (holds && in.distance_m > 0 && std::isfinite(in.distance_m)) s.progress += in.distance_m;
        complete = d.distance_to_drive_m > 0 && s.progress >= d.distance_to_drive_m;
        break;
      case FreeRoamChallengeType::kSkills:
        if (holds)
          for (const auto& k : in.skills)
            if (audio::iequals(k, d.feat_id)) s.progress += 1;
        complete = d.target_feat_count > 0 && s.progress >= d.target_feat_count;
        break;
      case FreeRoamChallengeType::kSpeedCamera: {
        for (const auto& f : in.flashes) {
          const bool listed = std::any_of(d.camera_ids.begin(), d.camera_ids.end(),
                                          [&](const std::string& c) { return audio::iequals(c, f.camera_id); });
          if (!listed) continue;
          // interim: the local flash is checked against the restrictions at
          // the flash's own speed; session players' flashes arrive checked.
          if (f.player == 0 && !d.restrictions_hold(in.tod_hour, f.speed_mph, in.car)) continue;
          s.flashes[lower(f.camera_id)][f.player] = in.time_s;
        }
        const double cutoff = in.time_s - d.time_limit_s;
        int count = 0;
        bool local = false;
        for (const auto& [cam, players] : s.flashes) {
          int n = 0;
          for (const auto& [player, at] : players) {
            if (at < cutoff) continue;
            ++n;
            if (player == 0) local = true;
          }
          count += d.cameras_must_be_unique ? (n > 0 ? 1 : 0) : n;
        }
        complete = local && d.num_cameras_to_flash > 0 && count >= d.num_cameras_to_flash;
        break;
      }
      case FreeRoamChallengeType::kDriveFromAToB:
        // min_players (2 or more) must make the trip in an online session;
        // the engine has none, so the trip is never tracked.
      case FreeRoamChallengeType::kUnknown:
        break;
    }
    if (complete) {
      s.completed = true;
      done.push_back({d.id, d.reward_credits, &d});
    }
  }
  return done;
}

bool FreeRoamChallengeTracker::completed(std::string_view id) const {
  for (const auto& [k, s] : states_)
    if (audio::iequals(k, id)) return s.completed;
  return false;
}

double FreeRoamChallengeTracker::progress(std::string_view id) const {
  for (const auto& [k, s] : states_)
    if (audio::iequals(k, id)) return s.progress;
  return 0;
}

double FreeRoamChallengeTracker::fraction(std::string_view id) const {
  const FreeRoamChallengeDef* d = data_.find(id);
  if (!d) return 0;
  if (completed(id)) return 1;
  double target = 0;
  if (d->type == FreeRoamChallengeType::kDistanceDriven) target = d->distance_to_drive_m;
  if (d->type == FreeRoamChallengeType::kSkills) target = d->target_feat_count;
  return target > 0 ? std::clamp(progress(id) / target, 0.0, 1.0) : 0.0;
}

int FreeRoamChallengeTracker::completed_count() const {
  return int(std::count_if(states_.begin(), states_.end(), [](const auto& kv) { return kv.second.completed; }));
}

std::vector<ChallengeRecord> FreeRoamChallengeTracker::save() const {
  std::vector<ChallengeRecord> out;
  for (const auto& d : data_.challenges) {
    auto it = states_.find(d.id);
    if (it == states_.end() || (it->second.progress <= 0 && !it->second.completed)) continue;
    out.push_back({d.id, it->second.progress, it->second.completed});
  }
  return out;
}

void FreeRoamChallengeTracker::load(const std::vector<ChallengeRecord>& records) {
  states_.clear();
  for (const auto& r : records) {
    const FreeRoamChallengeDef* d = data_.find(r.id);
    if (!d) continue;  // a challenge the data no longer has
    State& s = states_[d->id];
    s.progress = std::isfinite(r.progress) ? std::max(0.0, r.progress) : 0.0;
    s.completed = r.completed;
  }
}

}  // namespace freeroam
