#include "race/career.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <functional>
#include <sstream>

#include "audio/xml.h"
#include "db_util.h"
#include "fh1data/archive.h"

namespace race {
namespace {

std::string lower_str(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

double col_d(sqlite3_stmt* s, int c) { return sqlite3_column_double(s, c); }
int64_t col_i64(sqlite3_stmt* s, int c) { return sqlite3_column_int64(s, c); }
std::string col_txt(sqlite3_stmt* s, int c) {
  const auto* t = sqlite3_column_text(s, c);
  return t ? reinterpret_cast<const char*>(t) : "";
}

bool read_zip_xml(fh1::Archive& zip, const std::string& member, audio::XmlNode& out) {
  auto r = zip.read(member);
  if (!r.ok() || r.data.empty()) return false;
  std::string_view sv(reinterpret_cast<const char*>(r.data.data()), r.data.size());
  return audio::parse_xml(sv, out, nullptr);
}

}  // namespace

const CareerEvent* CareerCatalog::find_event(const std::string& horizon_id) const {
  for (const auto& e : events)
    if (e.horizon_id == horizon_id) return &e;
  return nullptr;
}

const CareerEvent* CareerCatalog::find_event_by_id(int64_t event_id) const {
  for (const auto& e : events)
    if (e.event_id == event_id) return &e;
  return nullptr;
}

const StreetHub* CareerCatalog::find_hub(int hub_id) const {
  for (const auto& h : hubs)
    if (h.id == hub_id) return &h;
  return nullptr;
}

const CarSpec* CareerCatalog::find_car(int64_t car_id) const {
  auto it = cars.find(car_id);
  return it == cars.end() ? nullptr : &it->second;
}

const CarSpec* CareerCatalog::find_car_by_media(const std::string& media) const {
  auto it = car_id_by_media.find(lower_str(media));
  return it == car_id_by_media.end() ? nullptr : find_car(it->second);
}

const FestivalSite* CareerCatalog::find_site(FestivalSite::Kind kind) const {
  for (const auto& s : sites)
    if (s.kind == kind) return &s;
  return nullptr;
}

int CareerCatalog::earned_wristband(int wristband_points) const {
  int best = -1;
  for (size_t i = 0; i < wristbands.size(); ++i)
    if (wristband_points >= wristbands[i].xp) best = int(i);
  return best;
}

int CareerCatalog::pending_wristband(int wristband_points, int collected_wristband) const {
  const int earned = earned_wristband(wristband_points);
  if (earned <= collected_wristband) return -1;
  return std::min(collected_wristband + 1, int(wristbands.size()) - 1);
}

bool CareerCatalog::is_hub_unlocked(const StreetHub& hub, const CareerRecord& rec, int collected_wristband) const {
  // Hub 1 unlocks at 310 pts (Green), Hub 2 at 1700 (Pink), Hub 3 at 4500 (Purple).
  if (rec.wristband_points < hub.unlock_points_req) return false;
  return collected_wristband >= 0;
}

bool CareerCatalog::is_event_unlocked(const CareerEvent& ev, const CareerRecord& rec, int collected_wristband,
                                      int popularity_rank) const {
  if (ev.prereq_event_id > 0) {
    auto it = rec.event_places.find(ev.prereq_event_id);
    if (it == rec.event_places.end() || it->second != 1) return false;
  }
  switch (ev.style) {
    case 1:  // Festival race
      if (ev.level < 0) return rec.wristband_points >= ev.unlock_points_req;
      return collected_wristband >= ev.level && rec.wristband_points >= ev.unlock_points_req;
    case 2: {  // Street race
      if (const StreetHub* hub = find_hub(ev.hub_id))
        if (!is_hub_unlocked(*hub, rec, collected_wristband)) return false;
      return rec.wristband_points >= ev.unlock_points_req;
    }
    case 3:  // Showcase (Popularity rank gate: 250..1)
      if (ev.popularity_rank_req <= 0 || ev.popularity_rank_req > 250) return false;
      return popularity_rank <= ev.popularity_rank_req;
    case 5:  // Nemesis / Star Showdown
      return collected_wristband >= std::max(0, ev.level) && rec.wristband_points >= ev.unlock_points_req;
    case 6:  // Horizon Finale (Headline event)
      return collected_wristband >= 6 && rec.wristband_points >= ev.unlock_points_req;
    default:
      return false;
  }
}

bool CareerCatalog::is_event_completed(const CareerEvent& ev, const CareerRecord& rec) const {
  return rec.event_places.find(ev.event_id) != rec.event_places.end();
}

int CareerCatalog::best_place(const CareerEvent& ev, const CareerRecord& rec) const {
  auto it = rec.event_places.find(ev.event_id);
  return it == rec.event_places.end() ? 0 : it->second;
}

EligibilityResult CareerCatalog::check_eligibility(const CareerEvent& ev, int64_t car_id, int car_class_id,
                                                   const std::vector<std::string>& class_names) const {
  EligibilityResult r;
  if (ev.player_car_id > 0) {
    r.eligible = true;
    r.uses_loaner = true;
    r.loaner_car_id = ev.player_car_id;
    return r;
  }
  if (ev.target_class > 0 && car_class_id > ev.target_class) {
    r.eligible = false;
    const std::string cname =
        ev.target_class >= 0 && size_t(ev.target_class) < class_names.size() ? class_names[size_t(ev.target_class)] : "";
    r.reason = cname.empty() ? "CAR CLASS TOO HIGH FOR THIS EVENT" : "REQUIRES CLASS " + cname + " OR LOWER";
    return r;
  }
  const auto& bucket = ev.restrictions.buckets[0];
  if (!bucket.empty()) {
    const CarSpec* spec = find_car(car_id);
    if (!spec) {
      r.eligible = false;
      r.reason = "CAR DOES NOT MEET EVENT RESTRICTIONS";
      return r;
    }
    if (!shift::data::car_passes(bucket, spec->props)) {
      r.eligible = false;
      if (bucket.drive_type) {
        const char* dt = *bucket.drive_type == shift::data::kDriveFwd   ? "FWD"
                         : *bucket.drive_type == shift::data::kDriveRwd ? "RWD"
                                                                        : "AWD";
        r.reason = std::string("REQUIRES ") + dt + " DRIVETRAIN";
      } else if (bucket.year_min || bucket.year_max) {
        if (bucket.year_min && bucket.year_max)
          r.reason = "REQUIRES " + std::to_string(*bucket.year_min) + "-" + std::to_string(*bucket.year_max) + " MODEL";
        else if (bucket.year_min)
          r.reason = "REQUIRES " + std::to_string(*bucket.year_min) + " OR NEWER";
        else
          r.reason = "REQUIRES " + std::to_string(*bucket.year_max) + " OR OLDER";
      } else if (bucket.value_max) {
        r.reason = "REQUIRES CAR UNDER " + std::to_string(*bucket.value_max) + " CR";
      } else {
        r.reason = "CAR DOES NOT MEET EVENT RESTRICTIONS";
      }
      return r;
    }
  }
  return r;
}

const char* CareerCatalog::event_map_tag(const CareerEvent& ev) {
  switch (ev.style) {
    case 2: return "streetrace";
    case 3: return "exhibition";
    case 5: return "nemesisrace";
    case 6: return "race";
    default: return "race";
  }
}

const char* CareerCatalog::event_style_label(const CareerEvent& ev) {
  switch (ev.style) {
    case 1: return "FESTIVAL RACE";
    case 2: return "STREET RACE";
    case 3: return "SHOWCASE";
    case 5: return "STAR SHOWDOWN";
    case 6: return "HORIZON FINALE";
    default: return "EVENT";
  }
}

bool load_career_catalog(const std::filesystem::path& game_root, CareerCatalog& out, std::string* error) {
  return load_career_catalog(game_root, {}, nullptr, out, error);
}

bool load_career_catalog(const std::filesystem::path& game_root, const std::filesystem::path& state_root,
                         const cars::PartsDb* shared_db, CareerCatalog& out, std::string* error) {
  out = CareerCatalog{};
  detail::RaceDb db;
  if (!db.open(game_root, state_root, shared_db, error)) return false;

  // 1. Wristband levels + wristband car rewards.
  std::map<int, int64_t> wb_cars;
  if (!db.each("select WristbandLevel, RewardCarId from Rewards_Wristband", {},
               [&](sqlite3_stmt* s) { wb_cars[int(col_i64(s, 0))] = col_i64(s, 1); }, error))
    return false;

  if (!db.each("select Id, XP, coalesce(Name, ''), coalesce(Thumbnail, ''), coalesce(Color, 0) "
               "from CareerWristbandLevels order by XP",
               {},
               [&](sqlite3_stmt* s) {
                 WristbandTier w;
                 w.id = int(col_i64(s, 0));
                 w.xp = int(col_i64(s, 1));
                 w.name_ref = col_txt(s, 2);
                 w.colour_key = col_txt(s, 3);
                 w.argb = uint32_t(col_i64(s, 4));
                 if (auto it = wb_cars.find(w.id); it != wb_cars.end()) w.reward_car_id = it->second;
                 out.wristbands.push_back(std::move(w));
               },
               error))
    return false;

  // 2. Prize cars and event-unlock prerequisites.
  std::map<int64_t, std::pair<int64_t, int>> prize_cars;
  if (!db.each("select EventId, RewardCarId, coalesce(RewardCarColorSequence, 0) "
               "from Rewards_EventPrizes order by Id",
               {},
               [&](sqlite3_stmt* s) {
                 const int64_t eid = col_i64(s, 0);
                 if (!prize_cars.count(eid)) prize_cars[eid] = {col_i64(s, 1), int(col_i64(s, 2))};
               },
               error))
    return false;

  std::map<int64_t, int64_t> prereqs;  // unlocked_event_id -> prerequisite_event_id
  if (!db.each("select EventId, UnlockEventId from Rewards_EventUnlock", {},
               [&](sqlite3_stmt* s) { prereqs[col_i64(s, 1)] = col_i64(s, 0); }, error))
    return false;

  // 3. Event restrictions.
  std::map<int64_t, shift::data::EventRestrictions> restrictions;
  if (!db.each("select EventID, BucketId, coalesce(Value, '') from EventRestrictions", {},
               [&](sqlite3_stmt* s) {
                 const std::string val = col_txt(s, 2);
                 if (val.empty()) return;
                 shift::data::set_restriction_bucket(restrictions[col_i64(s, 0)], col_i64(s, 1), val);
               },
               error))
    return false;

  // 4. Street race hubs.
  if (!db.each("select Id, coalesce(Name, ''), coalesce(UnlockPointsReq, 0) from EventHubs order by Id", {},
               [&](sqlite3_stmt* s) {
                 StreetHub h;
                 h.id = int(col_i64(s, 0));
                 h.name_ref = col_txt(s, 1);
                 h.unlock_points_req = int(col_i64(s, 2));
                 out.hubs.push_back(std::move(h));
               },
               error))
    return false;
  if (!db.each("select HubId, EventId from EventHubInitialEvents", {},
               [&](sqlite3_stmt* s) {
                 const int hid = int(col_i64(s, 0));
                 const int64_t eid = col_i64(s, 1);
                 for (auto& h : out.hubs)
                   if (h.id == hid) h.initial_events.push_back(eid);
               },
               error))
    return false;

  // 5. Playable career events (joined with Races & Tracks).
  if (!db.each("select e.Id, coalesce(e.HorizonEventID, ''), coalesce(e.Name, ''), "
               "coalesce(e.RestrictionDescription, ''), coalesce(e.Level, 0), coalesce(e.CareerEventStyle, 0), "
               "coalesce(e.CareerTypeId, 0), coalesce(e.UnlockPointsReq, 0), coalesce(e.PopularityPointsReq, 0), "
               "coalesce(e.HubId, 0), coalesce(e.TargetClass, 0), coalesce(e.CashPrize, 0), "
               "coalesce(e.PlayerCarId, 0), r.NumLaps, t.RouteId, t.RibbonConfiguration, "
               "(select count(*) from EventParticipants p where p.EventID = e.Id) "
               "from Events e join Races r on r.EventId = e.Id join Tracks t on t.id = r.TrackId "
               "where t.RibbonConfiguration >= 0 and e.CareerEventStyle > 0 "
               "and t.EnvironmentId != 111 and e.CareerTypeId != 9 "
               "order by e.Level, e.UnlockPointsReq, e.PopularityPointsReq desc, e.HorizonEventID",
               {},
               [&](sqlite3_stmt* s) {
                 CareerEvent ev;
                 ev.event_id = col_i64(s, 0);
                 ev.horizon_id = col_txt(s, 1);
                 ev.name_ref = col_txt(s, 2);
                 ev.restriction_desc_ref = col_txt(s, 3);
                 ev.level = int(col_i64(s, 4));
                 ev.style = int(col_i64(s, 5));
                 ev.career_type = int(col_i64(s, 6));
                 ev.unlock_points_req = int(col_i64(s, 7));
                 ev.popularity_rank_req = int(col_i64(s, 8));
                 ev.hub_id = int(col_i64(s, 9));
                 ev.target_class = int(col_i64(s, 10));
                 ev.cash_prize = col_d(s, 11);
                 ev.player_car_id = col_i64(s, 12);
                 ev.laps = std::max(1, int(col_i64(s, 13)));
                 ev.route_id = int(col_i64(s, 14));
                 ev.ribbon = int(col_i64(s, 15));
                 ev.opponents = int(col_i64(s, 16));
                 if (auto it = prize_cars.find(ev.event_id); it != prize_cars.end()) {
                   ev.prize_car_id = it->second.first;
                   ev.prize_car_colour = it->second.second;
                 }
                 if (auto it = prereqs.find(ev.event_id); it != prereqs.end()) ev.prereq_event_id = it->second;
                 if (auto it = restrictions.find(ev.event_id); it != restrictions.end()) ev.restrictions = it->second;
                 out.events.push_back(std::move(ev));
               },
               error))
    return false;

  // 6. Car specs for restriction checks.
  if (!db.each("select Id, coalesce(MediaName, ''), coalesce(Year, 0), coalesce(BaseCost, 0), "
               "coalesce(PerformanceIndex, 0), coalesce(ClassID, 0), coalesce(MakeID, 0), "
               "coalesce(DriveTypeID, 0), coalesce(FamilyBodyId, 0), coalesce(FamilyModelId, 0), "
               "coalesce(RegionId, 0), coalesce(CountryId, 0) from Data_Car",
               {},
               [&](sqlite3_stmt* s) {
                 CarSpec cs;
                 cs.car_id = col_i64(s, 0);
                 cs.media_name = col_txt(s, 1);
                 cs.props.year = uint32_t(std::max<int64_t>(0, col_i64(s, 2)));
                 cs.base_cost = int(col_i64(s, 3));
                 cs.props.value = uint32_t(std::max(0, cs.base_cost));
                 cs.stock_pi = col_d(s, 4);
                 cs.stock_class = int(col_i64(s, 5));
                 cs.props.make = uint16_t(std::max<int64_t>(0, col_i64(s, 6)));
                 cs.props.drive_type = uint8_t(std::max<int64_t>(0, col_i64(s, 7)));
                 cs.props.body_family = uint8_t(std::max<int64_t>(0, col_i64(s, 8)));
                 cs.props.model_family = uint16_t(std::max<int64_t>(0, col_i64(s, 9)));
                 cs.props.region = uint8_t(std::max<int64_t>(0, col_i64(s, 10)));
                 cs.props.country = uint8_t(std::max<int64_t>(0, col_i64(s, 11)));
                 if (!cs.media_name.empty()) out.car_id_by_media[lower_str(cs.media_name)] = cs.car_id;
                 out.cars[cs.car_id] = std::move(cs);
               },
               error))
    return false;

  // 7. World placements from GameObjs.xml + gamemodes.zip (non-fatal if absent in synthetic tests).
  std::map<std::string, std::array<double, 3>> game_objs;
  {
    const auto objs_path = game_root / "media" / "tracks" / "colorado" / "Ribbon_00" / "GameObjs.xml";
    std::ifstream f(objs_path, std::ios::binary);
    if (f) {
      std::stringstream ss;
      ss << f.rdbuf();
      audio::XmlNode root;
      if (audio::parse_xml(ss.str(), root, nullptr)) {
        for (const auto& o : root.children) {
          if (!o.has("GameplayID")) continue;
          if (const auto* pos = o.child("Pos"))
            game_objs[lower_str(o.attr("GameplayID"))] = {pos->number("x"), pos->number("y"), pos->number("z")};
        }
      }
    }
  }

  if (!game_objs.empty()) {
    auto modes = fh1::Archive::open(game_root / "media" / "gamemodes.zip", nullptr);
    if (modes) {
      // Festival sites.
      struct SiteDef {
        FestivalSite::Kind kind;
        const char* xml;
        const char* label;
        const char* tag;
      };
      const SiteDef site_defs[] = {
          {FestivalSite::kRaceCentral, "Colorado/race_central.xml", "RACE CENTRAL", "racecentral"},
          {FestivalSite::kAutoshow, "Colorado/autoshow.xml", "AUTOSHOW", "autoshow"},
          {FestivalSite::kWorkshop, "Colorado/workshops.xml", "DAK'S GARAGE", "workshop"},
          {FestivalSite::kPaintShop, "Colorado/paintshop.xml", "PAINT SHOP", "paintshop"},
      };
      for (const auto& sd : site_defs) {
        audio::XmlNode doc;
        if (!read_zip_xml(*modes, sd.xml, doc)) continue;
        for (const auto& act : doc.children) {
          if (!audio::iequals(act.name, "Activity")) continue;
          const auto* tz = act.child("TriggerZone");
          if (!tz) continue;
          auto it = game_objs.find(lower_str(tz->attr("object")));
          if (it == game_objs.end()) continue;
          FestivalSite site;
          site.kind = sd.kind;
          site.name = sd.label;
          site.map_tag = tz->has("mapTag") ? tz->attr("mapTag") : sd.tag;
          site.pos = it->second;
          site.radius = std::max(18.0, tz->number("radius", 24.0));
          out.sites.push_back(std::move(site));
          break;
        }
      }

      // Street race hubs.
      {
        audio::XmlNode doc;
        if (read_zip_xml(*modes, "Colorado/street_race_hubs.xml", doc)) {
          for (const auto& act : doc.children) {
            if (!audio::iequals(act.name, "Activity")) continue;
            const std::string name = lower_str(act.attr("name"));
            if (name.rfind("eventhub", 0) != 0) continue;
            const int hid = std::atoi(name.c_str() + 8);
            const auto* tz = act.child("TriggerZone");
            if (!tz) continue;
            auto it = game_objs.find(lower_str(tz->attr("object")));
            if (it == game_objs.end()) continue;
            for (auto& h : out.hubs)
              if (h.id == hid) {
                h.has_trigger = true;
                h.trigger_pos = it->second;
                h.trigger_radius = std::max(22.0, tz->number("radius", 24.0));
              }
          }
        }
      }

      // Career event activations.
      {
        audio::XmlNode doc;
        if (read_zip_xml(*modes, "Colorado/career_event_activations.xml", doc)) {
          std::map<std::string, std::pair<std::array<double, 3>, double>> act_pos;
          for (const auto& act : doc.children) {
            if (!audio::iequals(act.name, "Activity")) continue;
            const std::string name = lower_str(act.attr("name"));
            const auto* tz = act.child("TriggerZone");
            if (!tz) continue;
            auto it = game_objs.find(lower_str(tz->attr("object")));
            if (it == game_objs.end()) it = game_objs.find(name);
            if (it == game_objs.end()) continue;
            act_pos[name] = {it->second, std::max(18.0, tz->number("radius", 25.0))};
          }
          for (auto& ev : out.events) {
            if (ev.style == 2) continue;  // Street races launch from their Street Hub
            const std::string key = lower_str(ev.horizon_id);
            if (auto it = act_pos.find(key); it != act_pos.end()) {
              ev.has_trigger = true;
              ev.trigger_pos = it->second.first;
              ev.trigger_radius = it->second.second;
            } else if (auto jt = game_objs.find(key); jt != game_objs.end()) {
              ev.has_trigger = true;
              ev.trigger_pos = jt->second;
              ev.trigger_radius = 25.0;
            } else if (auto kt = game_objs.find(key + "_00"); kt != game_objs.end()) {
              ev.has_trigger = true;
              ev.trigger_pos = kt->second;
              ev.trigger_radius = 25.0;
            }
          }
        }
      }
    }
  }
  return true;
}

}  // namespace race
