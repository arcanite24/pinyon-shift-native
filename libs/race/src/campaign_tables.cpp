// The campaign's wristband, Star Showdown and Showcase tables, read from
// the disc (see campaign_flow.h). The built-in copy below is what the disc
// holds (gamedb.slt and the English string tables), kept for runs without
// one; progression_tests --corpus checks the two agree.
#include <sqlite3.h>

#include <array>
#include <cctype>
#include <iterator>
#include <map>

#include "db_util.h"
#include "fh1data/stringtables.h"
#include "race/campaign_flow.h"
#include "race/showcase.h"

namespace race {
namespace {

std::string lower_ascii(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string upper_ascii(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = char(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

// The story wiring the database does not hold: who each Star Showdown's
// rival is in the story and their VO cues, by HorizonEventID.
struct BossStory {
  const char* horizon_id;
  const char* key;
  CharacterId character;
  const char* vo_unlocked;
  const char* vo_pre_race;
  const char* vo_post_win;
  bool finale;
};
constexpr BossStory kBossStory[] = {
    {"NEM_TEST", "ramona", CharacterId::kRamonaCravache, "NemesisEventUnlocked1", "NemesisPreRace1",
     "NemesisPostRaceWin1", false},
    {"NR02", "ali", CharacterId::kAliHoward, "NemesisEventUnlocked2", "NemesisPreRace2", "NemesisPostRaceWin2", false},
    {"NR03", "duke", CharacterId::kDukeMaguire, "NemesisEventUnlocked3", "NemesisPreRace3", "NemesisPostRaceWin3",
     false},
    {"NR04", "marko", CharacterId::kMarkoBaran, "NemesisEventUnlocked4", "NemesisPreRace4", "NemesisPostRaceWin4",
     false},
    {"NR05", "zaki", CharacterId::kZakiMalik, "NemesisEventUnlocked5", "NemesisPreRace5", "NemesisPostRaceWin5", false},
    {"NR06", "hailey", CharacterId::kHaileyHarper, "NemesisEventUnlocked6", "NemesisPreRace6", "NemesisPostRaceWin6",
     false},
    // Opens from a HEADLINE_EVENT win (Rewards_EventUnlock 167 -> 169), not from points.
    {"NEM_FINAL", "darius", CharacterId::kDariusFlynt, "NemesisEventUnlocked7", "NemesisPreRace7",
     "NemesisPostRaceWin7", true},
};

const BossStory* boss_story(std::string_view horizon_id) {
  const std::string want = upper_ascii(horizon_id);
  for (const auto& b : kBossStory)
    if (want == b.horizon_id) return &b;
  return nullptr;
}

// The wristband VO cues, by level.
struct BandStory {
  const char* vo_unlock;
  const char* vo_new;
  const char* radio_event;
};
constexpr BandStory kBandStory[] = {
    {"FirstWristbandCollect", "NewWristbandUnlocked1", "AliceOpeningInterview2A"},
    {"GreenWristbandUnlock", "NewWristbandUnlocked2", ""},
    {"BlueWristbandUnlock", "NewWristbandUnlocked3", ""},
    {"PinkWristbandUnlock", "NewWristbandUnlocked4", ""},
    {"OrangeWristbandUnlock", "NewWristbandUnlocked5", ""},
    {"PurpleWristbandUnlock", "NewWristbandUnlocked6", ""},
    {"GoldWristbandUnlock", "NewWristbandUnlocked7", ""},
};

constexpr const char* kStarShowdown = "STAR SHOWDOWN";  // CareerEventUI IDS_TitleNemesisRace

WristbandMilestoneSpec band(int level, const char* colour, int xp, int64_t bonus, int festival_events) {
  WristbandMilestoneSpec w;
  w.level = level;
  w.key = lower_ascii(colour);
  w.colour_key = colour;
  w.display_name = upper_ascii(colour) + " WRISTBAND";
  w.xp_threshold = xp;
  w.nemesis_bonus = bonus;
  const auto& st = kBandStory[size_t(level)];
  w.vo_unlock_trigger = st.vo_unlock;
  w.vo_new_trigger = st.vo_new;
  w.radio_event = st.radio_event;
  w.unlocked_events_hint = festival_events;
  return w;
}

std::vector<WristbandMilestoneSpec> builtin_wristbands() {
  return {
      band(0, "Yellow", 0, 1000, 3),    band(1, "Green", 220, 1500, 7),   band(2, "Blue", 750, 2000, 8),
      band(3, "Pink", 1450, 2500, 9),   band(4, "Orange", 2500, 3000, 12), band(5, "Purple", 4100, 4000, 14),
      band(6, "Gold", 6400, 5000, 16),
  };
}

BossRivalSpec boss(int level, int64_t event_id, const char* horizon_id, const char* name, int64_t car_id,
                   const char* media, const char* car, int unlock_points, int64_t cash) {
  BossRivalSpec b;
  b.index = level;
  b.wristband_level = level;
  b.event_id = event_id;
  b.horizon_id = horizon_id;
  b.rival_name = name;
  b.rival_title = kStarShowdown;
  b.car_id = car_id;
  b.car_media = media;
  b.car_display = car;
  b.unlock_points = unlock_points;
  b.cash_prize = cash;
  if (const BossStory* st = boss_story(horizon_id)) {
    b.key = st->key;
    b.character = st->character;
    b.vo_unlocked = st->vo_unlocked;
    b.vo_pre_race = st->vo_pre_race;
    b.vo_post_win = st->vo_post_win;
    b.is_finale_champion = st->finale;
  }
  return b;
}

std::vector<BossRivalSpec> builtin_bosses() {
  return {
      boss(0, 79, "NEM_TEST", "Ramona Cravache", 1460, "FOR_SVTRaptor_12", "2011 Ford F-150 SVT Raptor", 220, 5000),
      boss(1, 241, "NR02", "Ali Howard", 1557, "DOD_ChallengerSRT8392_12", "2012 Dodge Challenger SRT8 392", 750,
           10000),
      boss(2, 242, "NR03", "Duke Maguire", 325, "LAM_DiabloSV_97", "1997 Lamborghini Diablo SV", 1450, 15000),
      boss(3, 243, "NR04", "Marko Baran", 1558, "NIS_GTR_12", "2012 Nissan GT-R Black Edition", 2500, 20000),
      boss(4, 244, "NR05", "Zaki Malik", 1260, "LEX_LFA_10", "2010 Lexus LF-A", 4100, 30000),
      boss(5, 245, "NR06", "Hailey Harper", 432, "SAL_S7_04", "2004 Saleen S7", 6400, 40000),
      boss(6, 169, "NEM_FINAL", "Darius Flynt", 1171, "FER_599XX_10", "2010 Ferrari 599XX", 0, 50000),
  };
}

ShowcaseMilestoneSpec showcase(int index, int64_t event_id, const char* horizon_id, const char* name,
                               const char* pilot, int rank, int64_t car_id, const char* media, const char* car,
                               int64_t cash) {
  ShowcaseMilestoneSpec s;
  s.index = index;
  s.event_id = event_id;
  s.horizon_id = horizon_id;
  s.display_name = name;
  s.opponent_label = pilot;
  s.popularity_rank_req = rank;
  s.loaner_car_id = car_id;
  s.loaner_car_media = media;
  s.loaner_car_display = car;
  s.cash_prize = cash;
  s.airborne = *pilot != '\0';
  return s;
}

std::vector<ShowcaseMilestoneSpec> builtin_showcases() {
  return {
      showcase(0, 101, "PLANE_RACE_001", "Mustang vs Mustang", "Plane", 250, 404, "FOR_MustangBOSS429_70",
               "1970 Ford Mustang Boss 429", 5000),
      showcase(1, 223, "PLANE_RACE_004", "High Noon Balloon", "Balloon", 225, 633, "AUD_SportQuattro_83",
               "1983 Audi Sport quattro", 6500),
      showcase(2, 211, "EXHIBITION_FEST_003", "Manic MINI", "", 200, 1566, "AUS_ShowCaseMiniCooperS_65",
               "1965 MINI Cooper S", 8500),
      showcase(3, 168, "PLANE_RACE_002", "The Wind Tunnel", "Chopper", 175, 1527, "CHE_CORVETTEGSC_10",
               "2010 Chevrolet Corvette Grand Sport Convertible", 10000),
      showcase(4, 246, "PLANE_RACE_007", "Bi-Plane Beatdown", "Biplane", 150, 340, "FER_F40_87", "1987 Ferrari F40",
               12500),
      showcase(5, 214, "EXHIBITION_RESE_003", "Scorpion Sting", "", 125, 1115, "FIA_500AbarthSS_10",
               "2010 Abarth 500 esseesse", 15000),
      showcase(6, 172, "PLANE_RACE_003", "The Stampede", "Biplane", 100, 1398, "LAM_LP7004_12",
               "2012 Lamborghini Aventador LP700-4", 17500),
      showcase(7, 222, "PLANE_RACE_006", "RUF 'n' Ready", "Plane", 75, 1432, "RUF_RGT8_11", "2011 RUF RGT-8", 20000),
      showcase(8, 59, "EXHIBITION_FEST_004", "Je m'appelle Awesome", "", 50, 1533, "PEU_205T16_84",
               "1984 Peugeot 205 Turbo 16", 22500),
      showcase(9, 220, "PLANE_RACE_005", "Agent of Speed", "Chopper", 1, 1181, "AST_One77_10",
               "2010 Aston Martin One-77", 25000),
  };
}

struct Tables {
  std::vector<WristbandMilestoneSpec> bands = builtin_wristbands();
  std::vector<BossRivalSpec> bosses = builtin_bosses();
  std::vector<ShowcaseMilestoneSpec> showcases = builtin_showcases();
};

Tables& tables() {
  static Tables t;
  return t;
}

std::string col_text(sqlite3_stmt* st, int col) {
  const unsigned char* s = sqlite3_column_text(st, col);
  return s ? reinterpret_cast<const char*>(s) : "";
}

// `_&N` for a string named `ids` in table `table`.
std::string string_ref(std::string_view table, std::string_view ids) {
  const uint32_t n = (uint32_t(fh1::string_hash(table)) << 16) | fh1::string_hash(ids);
  return "_&" + std::to_string(n);
}

struct Car {
  std::string media, display;
};

}  // namespace

const std::vector<WristbandMilestoneSpec>& wristband_milestones() { return tables().bands; }

const WristbandMilestoneSpec* find_wristband_milestone(int level) {
  for (const auto& b : tables().bands)
    if (b.level == level) return &b;
  return nullptr;
}

const std::vector<BossRivalSpec>& boss_rivals() { return tables().bosses; }

const BossRivalSpec* find_boss_by_event(std::string_view horizon_id) {
  const std::string want = upper_ascii(horizon_id);
  for (const auto& b : tables().bosses)
    if (upper_ascii(b.horizon_id) == want) return &b;
  return nullptr;
}

const BossRivalSpec* find_boss_by_key(std::string_view key) {
  std::string want = lower_ascii(key);
  if (want == "haley") want = "hailey";
  for (const auto& b : tables().bosses)
    if (want == b.key || want == lower_ascii(b.horizon_id)) return &b;
  return nullptr;
}

const BossRivalSpec* find_boss_by_level(int wristband_level) {
  for (const auto& b : tables().bosses)
    if (b.wristband_level == wristband_level) return &b;
  return nullptr;
}

const std::vector<ShowcaseMilestoneSpec>& showcase_milestones() { return tables().showcases; }

const ShowcaseMilestoneSpec* find_showcase_by_event(std::string_view horizon_id) {
  const std::string want = upper_ascii(horizon_id);
  for (const auto& sc : tables().showcases)
    if (upper_ascii(sc.horizon_id) == want) return &sc;
  return nullptr;
}

void reset_campaign_tables() { tables() = Tables{}; }

bool load_campaign_tables(const std::filesystem::path& game_root, const std::filesystem::path& state_root,
                          const cars::PartsDb* shared_db, const std::function<std::string(std::string_view)>& text,
                          CampaignTablesLoad* counts, std::string* error) {
  detail::RaceDb db;
  if (!db.open(game_root, state_root, shared_db, error)) return false;
  CampaignTablesLoad n;
  const Tables builtin;

  // A string reference's text, or `fallback` (counted) when it does not resolve.
  auto caption = [&](const std::string& ref, const std::string& fallback) {
    if (ref.empty()) return fallback;
    if (!fh1::StringTables::is_reference(ref)) return ref;
    std::string t = text ? text(ref) : std::string();
    if (t.empty() || t == ref || fh1::StringTables::is_reference(t)) {
      ++n.captions_missing;
      return fallback;
    }
    return t;
  };

  // Car names: "<Year> <make> <model>".
  std::map<int64_t, std::array<std::string, 4>> car_rows;  // media, year, make ref, model ref
  if (!db.each("select c.Id, c.MediaName, c.Year, coalesce(m.DisplayName, ''), coalesce(c.DisplayName, '') "
               "from Data_Car c left join List_CarMake m on m.ID = c.MakeID",
               {},
               [&](sqlite3_stmt* s) {
                 car_rows[sqlite3_column_int64(s, 0)] = {col_text(s, 1), col_text(s, 2), col_text(s, 3),
                                                         col_text(s, 4)};
               },
               error))
    return false;
  auto name_car = [&](int64_t id, const std::string& fb_media, const std::string& fb_display) {
    auto it = car_rows.find(id);
    if (it == car_rows.end()) return Car{fb_media, fb_display};
    const auto& r = it->second;
    const std::string make = caption(r[2], "");
    const std::string model = caption(r[3], "");
    if (make.empty() || model.empty()) return Car{r[0], fb_display.empty() ? r[0] : fb_display};
    const std::string year = r[1].empty() ? "" : r[1] + " ";
    return Car{r[0], year + make + " " + model};
  };

  // 1. Wristbands, and the Festival events at each level.
  std::map<int, int> festival;
  if (!db.each("select Level, count(*) from Events where CareerEventStyle = 1 and Level >= 0 group by Level", {},
               [&](sqlite3_stmt* s) { festival[sqlite3_column_int(s, 0)] = sqlite3_column_int(s, 1); }, error))
    return false;
  const std::string band_format = caption(string_ref("Scoreboard", "IDS_FormattedWristband"), "{0} WRISTBAND");
  std::vector<WristbandMilestoneSpec> bands;
  if (!db.each("select Id, XP, coalesce(Name, ''), coalesce(Thumbnail, ''), coalesce(NemesisBonus, 0) "
               "from CareerWristbandLevels order by Id",
               {},
               [&](sqlite3_stmt* s) {
                 const int lv = sqlite3_column_int(s, 0);
                 if (lv < 0 || lv >= int(std::size(kBandStory))) return;
                 const std::string colour = col_text(s, 3);
                 WristbandMilestoneSpec w = band(lv, colour.c_str(), sqlite3_column_int(s, 1),
                                                 sqlite3_column_int64(s, 4), festival[lv]);
                 const WristbandMilestoneSpec* fb = size_t(lv) < builtin.bands.size() ? &builtin.bands[size_t(lv)]
                                                                                       : nullptr;
                 // The format is in capitals ("{0} WRISTBAND"), and so is the name in it.
                 const std::string name = upper_ascii(caption(col_text(s, 2), colour));
                 std::string disp = band_format;
                 if (auto at = disp.find("{0}"); at != std::string::npos) disp.replace(at, 3, name);
                 w.display_name = disp.empty() && fb ? fb->display_name : disp;
                 bands.push_back(std::move(w));
               },
               error))
    return false;

  // 2. Star Showdowns: CareerEventStyle 5, by level; the pink slip is the event's prize car.
  std::vector<BossRivalSpec> bosses;
  if (!db.each("select e.Id, e.HorizonEventID, e.Level, coalesce(e.Name, ''), coalesce(e.UnlockPointsReq, 0), "
               "coalesce(e.CashPrize, 0), coalesce(r.RewardCarId, 0) "
               "from Events e left join Rewards_EventPrizes r on r.EventId = e.Id "
               "where e.CareerEventStyle = 5 order by e.Level, e.Id",
               {},
               [&](sqlite3_stmt* s) {
                 const std::string hid = col_text(s, 1);
                 const BossRivalSpec* fb = nullptr;
                 for (const auto& b : builtin.bosses)
                   if (b.horizon_id == hid) fb = &b;
                 const int64_t car_id = sqlite3_column_int64(s, 6);
                 const Car car = name_car(car_id, fb ? fb->car_media : "", fb ? fb->car_display : "");
                 BossRivalSpec b = boss(sqlite3_column_int(s, 2), sqlite3_column_int64(s, 0), hid.c_str(),
                                        caption(col_text(s, 3), fb ? fb->rival_name : hid).c_str(), car_id,
                                        car.media.c_str(), car.display.c_str(), sqlite3_column_int(s, 4),
                                        sqlite3_column_int64(s, 5));
                 b.index = int(bosses.size());
                 if (b.key.empty()) b.key = lower_ascii(hid);
                 b.rival_title = caption(string_ref("CareerEventUI", "IDS_TitleNemesisRace"), kStarShowdown);
                 bosses.push_back(std::move(b));
               },
               error))
    return false;

  // 3. Showcases: CareerEventStyle 3, by popularity rank; the opponent is
  // airborne_challenges.xml's pilot.
  std::map<int64_t, std::string> pilots;
  {
    std::vector<ShowcaseSpec> specs;
    std::string xml_error;
    if (load_airborne_challenges(game_root, specs, &xml_error))
      for (const auto& sp : specs) pilots[sp.event_id] = sp.pilot_name_ids;
  }
  std::vector<ShowcaseMilestoneSpec> showcases;
  if (!db.each("select Id, HorizonEventID, coalesce(ShortName, ''), coalesce(PopularityPointsReq, 0), "
               "coalesce(PlayerCarId, 0), coalesce(CashPrize, 0) "
               "from Events where CareerEventStyle = 3 order by PopularityPointsReq desc, Id",
               {},
               [&](sqlite3_stmt* s) {
                 const int64_t id = sqlite3_column_int64(s, 0);
                 const std::string hid = col_text(s, 1);
                 const ShowcaseMilestoneSpec* fb = nullptr;
                 for (const auto& sc : builtin.showcases)
                   if (sc.horizon_id == hid) fb = &sc;
                 const int64_t car_id = sqlite3_column_int64(s, 4);
                 const Car car = name_car(car_id, fb ? fb->loaner_car_media : "", fb ? fb->loaner_car_display : "");
                 std::string pilot;
                 bool airborne = false;
                 if (auto it = pilots.find(id); it != pilots.end()) {
                   airborne = true;
                   pilot = it->second.empty() ? "" : caption(string_ref("AirChallenges", it->second),
                                                             fb ? fb->opponent_label : "");
                 }
                 ShowcaseMilestoneSpec sc =
                     showcase(int(showcases.size()), id, hid.c_str(), caption(col_text(s, 2), fb ? fb->display_name : hid).c_str(),
                              pilot.c_str(), sqlite3_column_int(s, 3), car_id, car.media.c_str(), car.display.c_str(),
                              sqlite3_column_int64(s, 5));
                 sc.airborne = airborne || (pilots.empty() && fb && fb->airborne);
                 showcases.push_back(std::move(sc));
               },
               error))
    return false;

  if (bands.empty() || bosses.empty()) {
    if (error) *error = "gamedb: no wristband levels or Star Showdowns";
    return false;
  }
  n.wristbands = int(bands.size());
  n.bosses = int(bosses.size());
  n.showcases = int(showcases.size());
  auto& t = tables();
  t.bands = std::move(bands);
  t.bosses = std::move(bosses);
  t.showcases = std::move(showcases);
  if (counts) *counts = n;
  return true;
}

}  // namespace race
