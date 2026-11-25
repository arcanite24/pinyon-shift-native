#include "race/reset_rules.h"

#include <cmath>
#include <cstdlib>

#include "audio/xml.h"
#include "cars/physics.h"
#include "fh1data/archive.h"

namespace race {
namespace {

bool fail(std::string* error, const std::string& what) {
  if (error) *error = what;
  return false;
}

bool read_member(const std::filesystem::path& game_root, std::string_view member, std::string& text,
                 std::string* error) {
  const auto zip = game_root / "media" / "gametunablesettings.zip";
  std::string why;
  auto a = fh1::Archive::open(zip, &why);
  if (!a) return fail(error, zip.string() + ": " + why);
  auto r = a->read(member);
  if (!r.ok()) return fail(error, zip.string() + ": " + std::string(member) + ": " + r.error);
  text.assign(r.data.begin(), r.data.end());
  return true;
}

}  // namespace

bool parse_hand_of_god_reset(std::string_view ini_text, HandOfGodReset& out, std::string* error) {
  cars::PhysicsSettings s;
  std::string why;
  if (!cars::parse_physics_settings(ini_text, s, &why)) return fail(error, "GameTunableSettings.ini: " + why);
  out.missing.clear();
  int found = 0;
  auto g = [&](const char* key, double& v) {
    const std::string k = std::string("HandOfGodReset\\") + key;
    const auto it = s.values.find(k);
    if (it == s.values.end()) {
      out.missing.push_back(k);
      return;
    }
    v = it->second;
    ++found;
  };
  g("FacingSpeedThresholdMPH", out.facing_speed_mph);
  g("FacingCosAngle", out.facing_cos);
  g("FacingTimeBeforeReset", out.facing_time);
  g("ProgressThresholdMPH", out.progress_mph);
  g("ProgressCosAngle", out.progress_cos);
  g("ProgressTimeBeforeReset", out.progress_time);
  g("OffTrackAmountMeters0", out.off_track_m0);
  g("OffTrackTimeBeforeReset0", out.off_track_time0);
  g("OffTrackAmountMeters1", out.off_track_m1);
  g("OffTrackTimeBeforeReset1", out.off_track_time1);
  g("PostResetNonCollideTime", out.post_reset_non_collide);
  g("OptimalSpeedScale", out.optimal_speed_scale);
  g("FadeOutTime", out.fade_out);
  g("FadeInTime", out.fade_in);
  g("CollisionsOffTime", out.collisions_off);
  if (found == 0) return fail(error, "GameTunableSettings.ini: no [HandOfGodReset] keys");
  return true;
}

bool parse_ai_reset_rules(std::string_view airacing_xml, std::string_view set, AiResetRules& out,
                          std::string* error) {
  audio::XmlNode root;
  std::string why;
  if (!audio::parse_xml(airacing_xml, root, &why)) return fail(error, "AIRacing.xml: " + why);
  const audio::XmlNode* settings = nullptr;
  for (const auto& c : root.children)
    if (audio::iequals(c.name, "Settings") && audio::iequals(c.attr("name"), set)) settings = &c;
  if (!settings) return fail(error, "AIRacing.xml: no <Settings name=\"" + std::string(set) + "\">");
  const audio::XmlNode* rp = settings->find("Batch/MetaAIDriver/ResetParameters");
  if (!rp) return fail(error, "AIRacing.xml: set " + std::string(set) + " has no MetaAIDriver/ResetParameters");
  std::string absent;
  auto g = [&](const std::string& name, double& v) {
    if (!rp->has(name)) {
      absent += (absent.empty() ? "" : ", ") + name;
      return;
    }
    v = rp->number(name, 0);
  };
  for (int k = 0; k < 3; ++k) {
    const std::string base = "ResetIfStuckWithinN" + std::to_string(k + 1) + "Waypoints_";
    double count = 0;
    g(base + "NumWaypoints", count);
    out.stuck_waypoints[size_t(k)].count = int(std::lround(count));
    g(base + "MinTime", out.stuck_waypoints[size_t(k)].min_time);
  }
  g("ResetIfStuckWithinThisDistance_Distance_m", out.stuck_distance_m);
  g("ResetIfStuckWithinThisDistance_MinTime", out.stuck_distance_time);
  g("ResetIfGoingThisSlow_Speed_mpers", out.slow_speed);
  g("ResetIfGoingThisSlow_MinTime", out.slow_time);
  g("ResetIfGoingThisSlowUpsideDown_Speed_mpers", out.upside_down_speed);
  g("ResetIfGoingThisSlowUpsideDown_MinTime", out.upside_down_time);
  g("ResetIfGoingThisSlowUpsideDown_UpVectorYRolloverThreshold", out.upside_down_up_y);
  g("ResetIfFellThisLow", out.fell_low);
  g("CancelResetIfMovedThisFar_m", out.cancel_moved_m);
  if (!absent.empty())
    return fail(error, "AIRacing.xml: set " + std::string(set) + " ResetParameters lacks " + absent);
  return true;
}

bool load_hand_of_god_reset(const std::filesystem::path& game_root, HandOfGodReset& out, std::string* error) {
  std::string text;
  if (!read_member(game_root, "GameTunableSettings.ini", text, error)) return false;
  return parse_hand_of_god_reset(text, out, error);
}

bool load_ai_reset_rules(const std::filesystem::path& game_root, std::string_view set, AiResetRules& out,
                         std::string* error) {
  std::string text;
  if (!read_member(game_root, "AIRacing.xml", text, error)) return false;
  return parse_ai_reset_rules(text, set, out, error);
}

bool parse_ai_racing_set(std::string_view airacing_xml, std::string_view set, AiRacingSet& out,
                         std::string* error) {
  audio::XmlNode root;
  std::string why;
  if (!audio::parse_xml(airacing_xml, root, &why)) return fail(error, "AIRacing.xml: " + why);
  const audio::XmlNode* settings = nullptr;
  for (const auto& c : root.children)
    if (audio::iequals(c.name, "Settings") && audio::iequals(c.attr("name"), set)) settings = &c;
  if (!settings) return fail(error, "AIRacing.xml: no <Settings name=\"" + std::string(set) + "\">");
  const audio::XmlNode* dls = settings->find("Batch/DLS");
  const audio::XmlNode* pass = settings->find("Batch/PassingDriver");
  const audio::XmlNode* ctl = settings->find("Batch/PredictiveControllerRLH");
  if (!dls || !pass || !ctl)
    return fail(error, "AIRacing.xml: set " + std::string(set) + " lacks DLS, PassingDriver or PredictiveControllerRLH");
  std::string absent;
  auto g = [&](const audio::XmlNode* n, const char* name, double& v) {
    if (!n->has(name)) absent += (absent.empty() ? "" : ", ") + std::string(name);
    else v = n->number(name, 0);
  };
  AiRacingSet r;
  DlsSettings& d = r.dls;
  g(dls, "TimeToCollsionCost", d.time_to_collision_cost);
  g(dls, "OutOfBoundsCost", d.out_of_bounds_cost);
  g(dls, "OffTrackCost", d.off_track_cost);
  g(dls, "CarCost", d.car_cost);
  g(dls, "OffLineCost", d.off_line_cost);
  g(dls, "ChangePassSidesCost", d.change_pass_sides_cost);
  g(dls, "HeadOnPassWrongSideCost", d.head_on_wrong_side_cost);
  g(dls, "MaxAcceptableCost", d.max_acceptable_cost);
  g(dls, "ExtraCarWidthAI", d.extra_width_ai);
  g(dls, "ExtraCarWidthHuman", d.extra_width_human);
  g(dls, "ExtraCarWidthTraffic", d.extra_width_traffic);
  g(pass, "TimeBetweenDynamicLineUpdates", d.update_interval);
  if (!pass->has("ShouldIgnoreCarsNotApproached")) absent += (absent.empty() ? "" : ", ") + std::string("ShouldIgnoreCarsNotApproached");
  else d.ignore_cars_not_approached = audio::iequals(pass->attr("ShouldIgnoreCarsNotApproached"), "true");
  if (!pass->has("LegalSideOfRoad")) absent += (absent.empty() ? "" : ", ") + std::string("LegalSideOfRoad");
  else {
    const std::string side = pass->attr("LegalSideOfRoad");
    d.legal_side = audio::iequals(side, "Right") ? 2 : audio::iequals(side, "Left") ? 1 : 0;
  }
  g(ctl, "SpeedComputerSlopeModifier", r.slope_modifier);
  if (!ctl->has("DisableTrackEdgeAvoidance")) absent += (absent.empty() ? "" : ", ") + std::string("DisableTrackEdgeAvoidance");
  else r.disable_track_edge = audio::iequals(ctl->attr("DisableTrackEdgeAvoidance"), "true");
  g(ctl, "SpeedComputerSlopeCurvatureModifier", r.slope_curvature);
  if (!absent.empty()) return fail(error, "AIRacing.xml: set " + std::string(set) + " lacks " + absent);
  out = r;
  return true;
}

bool load_ai_racing_set(const std::filesystem::path& game_root, std::string_view set, AiRacingSet& out,
                        std::string* error) {
  std::string text;
  if (!read_member(game_root, "AIRacing.xml", text, error)) return false;
  return parse_ai_racing_set(text, set, out, error);
}

bool parse_global_registry(std::string_view xml, std::map<std::string, double>& out, std::string* error) {
  std::map<std::string, double> r;
  auto attr = [](std::string_view elem, std::string_view name, std::string& v) {
    const std::string key = " " + std::string(name) + "=\"";
    const size_t at = elem.find(key);
    if (at == std::string_view::npos) return false;
    const size_t from = at + key.size(), to = elem.find('"', from);
    if (to == std::string_view::npos) return false;
    v.assign(elem.substr(from, to - from));
    return true;
  };
  size_t at = 0;
  while ((at = xml.find("<Value", at)) != std::string_view::npos) {
    const size_t end = xml.find('>', at);
    if (end == std::string_view::npos) break;
    const std::string_view elem = xml.substr(at, end - at);
    at = end;
    std::string id, value;
    if (!attr(elem, "id", id) || !attr(elem, "value", value)) continue;
    char* stop = nullptr;
    const double v = std::strtod(value.c_str(), &stop);
    if (stop != value.c_str()) r[id] = v;
  }
  if (r.empty()) return fail(error, "GlobalRegistry.xml: no numeric <Value> entries");
  out = std::move(r);
  return true;
}

bool load_global_registry(const std::filesystem::path& game_root, std::map<std::string, double>& out,
                          std::string* error) {
  std::string text;
  if (!read_member(game_root, "GlobalRegistry.xml", text, error)) return false;
  return parse_global_registry(text, out, error);
}

const char* ai_settings_set(bool street_race, bool free_roam) {
  if (free_roam) return "freeroam";
  return street_race ? "streetrace" : "default";
}

}  // namespace race
