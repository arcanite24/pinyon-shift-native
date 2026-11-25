#include "freeroam/tunables.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "fh1data/archive.h"

namespace freeroam {
namespace {

// The value of attribute `name` inside one element's text, or false.
bool attr(std::string_view elem, std::string_view name, std::string& out) {
  const std::string key = std::string(name) + "=\"";
  size_t at = 0;
  while ((at = elem.find(key, at)) != std::string_view::npos) {
    // A whole attribute name: preceded by whitespace.
    if (at > 0 && elem[at - 1] != ' ' && elem[at - 1] != '\t' && elem[at - 1] != '\n' && elem[at - 1] != '\r') {
      at += key.size();
      continue;
    }
    const size_t from = at + key.size();
    const size_t to = elem.find('"', from);
    if (to == std::string_view::npos) return false;
    out.assign(elem.substr(from, to - from));
    return true;
  }
  return false;
}

}  // namespace

double Registry::get(std::string_view id, double fallback) const {
  auto it = values.find(std::string(id));
  return it == values.end() ? fallback : it->second;
}

bool parse_registry(std::string_view xml, Registry& out, std::string* error) {
  Registry r;
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
    if (stop == value.c_str()) continue;  // a string value
    r.values[id] = v;
  }
  if (r.values.empty()) {
    if (error) *error = "GlobalRegistry: no numeric <Value> entries";
    return false;
  }
  out = std::move(r);
  return true;
}

bool load_registry(const std::filesystem::path& game_root, Registry& out, std::string* error) {
  std::string why;
  auto zip = fh1::Archive::open(game_root / "media" / "gametunablesettings.zip", &why);
  if (!zip) {
    if (error) *error = why;
    return false;
  }
  auto r = zip->read("GlobalRegistry.xml");
  if (!r.ok()) {
    if (error) *error = "GlobalRegistry.xml: " + r.error;
    return false;
  }
  return parse_registry(std::string_view(reinterpret_cast<const char*>(r.data.data()), r.data.size()), out, error);
}

FastTravelTuning FastTravelTuning::from(const Registry& r) {
  FastTravelTuning t;
  t.total_cost = int64_t(std::llround(r.get("Activities/Outposts/TotalCost", double(t.total_cost))));
  for (size_t n = 0; n < t.discount.size(); ++n)
    t.discount[n] =
        r.get("Activities/Outposts/Discount_" + std::to_string(n) + "_Missions", t.discount[n]);
  return t;
}

int64_t fast_travel_cost(const FastTravelTuning& t, int stunts_done, int stunts_total) {
  if (stunts_total > 0 && stunts_done >= stunts_total) return 0;
  const int n = std::clamp(stunts_done, 0, int(t.discount.size()) - 1);
  const int64_t off = int64_t(std::trunc(double(t.total_cost) * t.discount[size_t(n)]));
  return std::max<int64_t>(0, t.total_cost - off);
}

BarnFindTuning BarnFindTuning::from(const Registry& r) {
  BarnFindTuning t;
  t.max_idle_time_s = r.get("Activities/BarnFinds/MaxIdleTime", t.max_idle_time_s);
  t.min_distance_before_next_m = r.get("Activities/BarnFinds/MinDistanceBeforeNextSpawn", t.min_distance_before_next_m);
  t.min_speed_before_spawn_ms = r.get("Activities/BarnFinds/MinSpeedBeforeSpawn", t.min_speed_before_spawn_ms);
  t.min_time_before_spawn_s = r.get("Activities/BarnFinds/MinTimeBeforeSpawn", t.min_time_before_spawn_s);
  t.time_between_ticks_s = r.get("Activities/BarnFinds/TimeBetweenTicks", t.time_between_ticks_s);
  t.time_prompt_on_screen_s = r.get("Activities/BarnFinds/TimePromptOnScreen", t.time_prompt_on_screen_s);
  return t;
}

}  // namespace freeroam
