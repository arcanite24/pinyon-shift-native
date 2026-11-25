#include "freeroam/fast_travel.h"

#include <algorithm>
#include <cctype>

#include "audio/xml.h"
#include "fh1data/archive.h"
#include "freeroam/outposts.h"

namespace freeroam {

namespace {

bool fail(std::string* error, const std::string& what) {
  if (error) *error = what;
  return false;
}

std::string lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

}  // namespace

bool parse_fast_travel_tunables(std::string_view xml, FastTravelTunables& out, std::string* error) {
  audio::XmlNode root;
  std::string why;
  if (!audio::parse_xml(std::string(xml), root, &why)) return fail(error, "GlobalRegistry.xml: " + why);
  // The values sit directly under the root element.
  const audio::XmlNode* reg = &root;
  if (reg->children.size() == 1 && !audio::iequals(reg->children[0].name, "Value")) reg = &reg->children[0];
  constexpr std::string_view kPrefix = "Activities/Outposts/";
  for (const auto& v : reg->children) {
    if (!audio::iequals(v.name, "Value") || !v.has("id")) continue;
    const std::string id = v.attr("id");
    if (id.rfind(kPrefix, 0) != 0) continue;
    const std::string key = id.substr(kPrefix.size());
    if (key == "TotalCost") {
      out.total_cost = int64_t(v.number("value", double(out.total_cost)));
    } else if (key.size() == std::string_view("Discount_0_Missions").size() && key.rfind("Discount_", 0) == 0 &&
               key.compare(10, std::string::npos, "_Missions") == 0) {
      const int n = key[9] - '0';
      if (n >= 0 && n < int(out.discount.size())) out.discount[size_t(n)] = float(v.number("value", out.discount[size_t(n)]));
    }
  }
  return true;
}

bool load_fast_travel_tunables(const std::filesystem::path& game_root, FastTravelTunables& out, std::string* error) {
  std::string why;
  auto zip = fh1::Archive::open(game_root / "media" / "gametunablesettings.zip", &why);
  if (!zip) return fail(error, why);
  auto reg = zip->read("GlobalRegistry.xml");
  if (!reg.ok()) return fail(error, "GlobalRegistry.xml: " + reg.error);
  return parse_fast_travel_tunables(std::string_view(reinterpret_cast<const char*>(reg.data.data()), reg.data.size()),
                                    out, error);
}

int64_t outpost_fast_travel_cost(const FastTravelTunables& t, int missions_total, int missions_done) {
  missions_done = std::max(0, missions_done);
  if (missions_done >= missions_total) return 0;
  const float frac = t.discount[size_t(std::min(missions_done, int(t.discount.size()) - 1))];
  // As the original: the discount truncated to whole credits, in float.
  const int64_t cost = t.total_cost - int64_t(float(t.total_cost) * frac);
  return std::max<int64_t>(0, cost);
}

int outpost_discount_percent(int missions_total, int missions_done) {
  missions_done = std::max(0, missions_done);
  if (missions_done >= missions_total) return 100;
  return missions_done * 25;
}

OutpostMissions outpost_missions(const OutpostCatalog& catalog, const OutpostProgress& progress,
                                 std::string_view outpost_id) {
  OutpostMissions m;
  const Outpost* op = catalog.find_outpost(outpost_id);
  if (!op) return m;
  if (!op->mission_ids.empty()) {
    m.total = int(op->mission_ids.size());
    for (const auto& id : op->mission_ids) m.done += progress.is_stunt_completed(id) ? 1 : 0;
  } else {
    for (const PrStunt* st : catalog.stunts_for_outpost(op->id)) {
      ++m.total;
      m.done += progress.is_stunt_completed(st->id) ? 1 : 0;
    }
  }
  return m;
}

FastTravelKind fast_travel_kind(std::string_view map_tag) {
  const std::string t = lower(map_tag);
  if (t == "gas_station") return FastTravelKind::kOutpost;
  if (t == "racecentral" || t == "workshop" || t == "autoshow" || t == "paintshop" || t == "carclub" ||
      t == "dlccenter")
    return FastTravelKind::kFree;
  return FastTravelKind::kNone;
}

FastTravelQuote quote_fast_travel(const FastTravelTunables& t, std::string_view map_tag, int missions_total,
                                  int missions_done, int64_t credits) {
  FastTravelQuote q;
  q.kind = fast_travel_kind(map_tag);
  if (q.kind == FastTravelKind::kOutpost) q.cost = outpost_fast_travel_cost(t, missions_total, missions_done);
  q.affordable = q.eligible() && credits >= q.cost;
  return q;
}

}  // namespace freeroam
