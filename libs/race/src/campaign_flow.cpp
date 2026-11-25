#include "race/campaign_flow.h"

#include "drive/save.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

namespace race {
namespace {

constexpr uint32_t pack_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
  return (uint32_t(a) << 24) | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
}

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

std::string trim_ascii(std::string_view s) {
  size_t b = 0;
  while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  size_t e = s.size();
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return std::string(s.substr(b, e - b));
}

bool looks_like_internal_event_key(std::string_view s) {
  if (s.empty()) return true;
  if (s.rfind("_&", 0) == 0) return true;
  const std::string up = upper_ascii(s);
  if (up.find('_') != std::string::npos) {
    bool all_key_chars = true;
    for (char c : up) {
      if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) {
        all_key_chars = false;
        break;
      }
    }
    if (all_key_chars) return true;
  }
  // Short coded keys like FR05, SR03, NR02, ST01, PR04.
  if (up.size() >= 3 && up.size() <= 6) {
    size_t alpha_n = 0;
    while (alpha_n < up.size() && std::isalpha(static_cast<unsigned char>(up[alpha_n]))) ++alpha_n;
    size_t digit_n = 0;
    while (alpha_n + digit_n < up.size() && std::isdigit(static_cast<unsigned char>(up[alpha_n + digit_n]))) ++digit_n;
    if (alpha_n >= 2 && alpha_n <= 3 && digit_n >= 1 && alpha_n + digit_n == up.size()) {
      const std::string pfx = up.substr(0, alpha_n);
      if (pfx == "FR" || pfx == "SR" || pfx == "NR" || pfx == "ST" || pfx == "PR" || pfx == "EV") return true;
    }
  }
  return false;
}

std::function<std::string(std::string_view)>& text_resolver_slot() {
  static std::function<std::string(std::string_view)> fn;
  return fn;
}

}  // namespace

// ---- Easing Curves ----

double ease_out_cubic(double t) {
  t = std::clamp(t, 0.0, 1.0);
  const double u = 1.0 - t;
  return 1.0 - u * u * u;
}

double ease_in_out_cubic(double t) {
  t = std::clamp(t, 0.0, 1.0);
  if (t < 0.5) return 4.0 * t * t * t;
  const double u = -2.0 * t + 2.0;
  return 1.0 - (u * u * u) * 0.5;
}

double ease_out_back(double t) {
  t = std::clamp(t, 0.0, 1.0);
  constexpr double c1 = 1.70158;
  constexpr double c3 = c1 + 1.0;
  const double u = t - 1.0;
  return 1.0 + c3 * u * u * u + c1 * u * u;
}

// ---- Stage Names ----

const char* campaign_stage_name(CampaignStage stage) {
  switch (stage) {
    case CampaignStage::kTitle: return "title";
    case CampaignStage::kOpeningCutscene: return "intro";
    case CampaignStage::kViperIntroDrive: return "viper_drive";
    case CampaignStage::kQualifyRace: return "qualify";
    case CampaignStage::kFestivalRegistration: return "registration";
    case CampaignStage::kFreeRoam: return "freeroam";
    case CampaignStage::kWristbandCeremony: return "wristband";
    case CampaignStage::kShowcaseIntro: return "showcase_intro";
    case CampaignStage::kShowcaseOutro: return "showcase_outro";
    case CampaignStage::kBossIntro: return "boss_intro";
    case CampaignStage::kBossVictory: return "boss_victory";
    case CampaignStage::kBarnDiscovery: return "barn_discovery";
    case CampaignStage::kBarnRestored: return "barn_restored";
    case CampaignStage::kPreRaceGrid: return "pre_race";
    case CampaignStage::kPostRaceFinish: return "post_race";
    case CampaignStage::kRewardTally: return "reward_tally";
    case CampaignStage::kFinaleIntro: return "finale_intro";
    case CampaignStage::kFinaleVictory: return "finale";
    case CampaignStage::kCredits: return "credits";
  }
  return "freeroam";
}

bool parse_campaign_stage(std::string_view name, CampaignStage& out) {
  const std::string k = lower_ascii(name);
  if (k == "title") { out = CampaignStage::kTitle; return true; }
  if (k == "intro" || k == "opening" || k == "opening_cutscene") { out = CampaignStage::kOpeningCutscene; return true; }
  if (k == "viper_drive" || k == "viper" || k == "intro_drive") { out = CampaignStage::kViperIntroDrive; return true; }
  if (k == "qualify" || k == "corrado_qualify") { out = CampaignStage::kQualifyRace; return true; }
  if (k == "registration" || k == "starter_handoff") { out = CampaignStage::kFestivalRegistration; return true; }
  if (k == "freeroam" || k == "free_roam") { out = CampaignStage::kFreeRoam; return true; }
  if (k == "wristband" || k.rfind("wristband_", 0) == 0) { out = CampaignStage::kWristbandCeremony; return true; }
  if (k == "showcase" || k == "showcase_intro") { out = CampaignStage::kShowcaseIntro; return true; }
  if (k == "showcase_outro") { out = CampaignStage::kShowcaseOutro; return true; }
  if (k == "boss" || k == "boss_intro" || k.rfind("boss_", 0) == 0) { out = CampaignStage::kBossIntro; return true; }
  if (k == "boss_victory") { out = CampaignStage::kBossVictory; return true; }
  if (k == "barn" || k == "barn_find" || k == "barn_discovery") { out = CampaignStage::kBarnDiscovery; return true; }
  if (k == "barn_restored") { out = CampaignStage::kBarnRestored; return true; }
  if (k == "pre_race" || k == "grid") { out = CampaignStage::kPreRaceGrid; return true; }
  if (k == "post_race" || k == "finish") { out = CampaignStage::kPostRaceFinish; return true; }
  if (k == "reward_tally" || k == "tally") { out = CampaignStage::kRewardTally; return true; }
  if (k == "finale_intro" || k == "darius_showdown") { out = CampaignStage::kFinaleIntro; return true; }
  if (k == "finale" || k == "finale_victory") { out = CampaignStage::kFinaleVictory; return true; }
  if (k == "credits") { out = CampaignStage::kCredits; return true; }
  return false;
}

// ---- Cast Profiles ----

const std::vector<CharacterProfile>& all_characters() {
  static const std::vector<CharacterProfile> kChars = {
      {CharacterId::kAliceHart, "alice", "ALICE HART", "HORIZON FESTIVAL DIRECTOR", pack_rgba(0xF9, 0x00, 0x64)},
      {CharacterId::kDakStewart, "dak", "DAK STEWART", "CHIEF MECHANIC  •  HORIZON GARAGE", pack_rgba(0xFF, 0xB8, 0x1C)},
      {CharacterId::kHollyCruz, "holly", "HOLLY CRUZ", "EVENT COORDINATOR  •  RACE CENTRAL", pack_rgba(0xDA, 0xFF, 0x00)},
      {CharacterId::kScottTyler, "scott", "SCOTT TYLER", "DJ  •  HORIZON BASS ARENA", pack_rgba(0x2C, 0x8C, 0xE8)},
      {CharacterId::kDariusFlynt, "darius", "DARIUS FLYNT", "REIGNING HORIZON CHAMPION", pack_rgba(0xFF, 0xD2, 0x3F)},
      {CharacterId::kRamonaCravache, "ramona", "RAMONA CRAVACHE", "YELLOW WRISTBAND STAR RIVAL", pack_rgba(0xFF, 0xD6, 0x00)},
      {CharacterId::kAliHoward, "ali", "ALI HOWARD", "GREEN WRISTBAND STAR RIVAL", pack_rgba(0x46, 0xDC, 0x6E)},
      {CharacterId::kDukeMaguire, "duke", "DUKE MAGUIRE", "BLUE WRISTBAND STAR RIVAL", pack_rgba(0x2C, 0x8C, 0xE8)},
      {CharacterId::kMarkoBaran, "marko", "MARKO BARAN", "PINK WRISTBAND STAR RIVAL", pack_rgba(0xF9, 0x00, 0x64)},
      {CharacterId::kZakiMalik, "zaki", "ZAKI MALIK", "ORANGE WRISTBAND STAR RIVAL", pack_rgba(0xFF, 0x7A, 0x00)},
      {CharacterId::kHaileyHarper, "hailey", "HAILEY HARPER", "PURPLE WRISTBAND STAR RIVAL", pack_rgba(0xA4, 0x45, 0xF5)},
  };
  return kChars;
}

const CharacterProfile& character_profile(CharacterId id) {
  const auto& chars = all_characters();
  const size_t idx = size_t(id);
  return idx < chars.size() ? chars[idx] : chars.front();
}

// ---- Wristband Milestones (Exact CareerWristbandLevels XP thresholds & gamedb.slt event counts) ----
// radio_event: an immediate DJ line; the later wristbands' DJ lines come from
// radiodj.xml (AudioHost::radio_wristband_awarded), not from here.

const std::vector<WristbandMilestoneSpec>& wristband_milestones() {
  static const std::vector<WristbandMilestoneSpec> kBands = {
      {0, "yellow", "YELLOW WRISTBAND", "Yellow", 0, 1000, "FirstWristbandCollect", "NewWristbandUnlocked1",
       "AliceOpeningInterview2A",
       "Welcome to the Horizon Festival! Yellow Wristband events, Race Central, and Dak's Garage are now open.", 3},
      {1, "green", "GREEN WRISTBAND", "Green", 220, 1500, "GreenWristbandUnlock", "NewWristbandUnlocked2",
       "",
       "Green Wristband unlocked! Carson Street Race Hub, faster Colorado events, and your next Star Showdown await.", 7},
      {2, "blue", "BLUE WRISTBAND", "Blue", 750, 2000, "BlueWristbandUnlock", "NewWristbandUnlocked3",
       "",
       "Blue Wristband unlocked! High-speed mountain passes and Duke Maguire's Star Showdown are now live.", 8},
      {3, "pink", "PINK WRISTBAND", "Pink", 1450, 2500, "PinkWristbandUnlock", "NewWristbandUnlocked4",
       "",
       "Pink Wristband unlocked! Red Rock canyon series, Gladstone Street Hub, and Marko Baran's GT-R challenge await.", 9},
      {4, "orange", "ORANGE WRISTBAND", "Orange", 2500, 3000, "OrangeWristbandUnlock", "NewWristbandUnlocked5",
       "",
       "Orange Wristband unlocked! Supercar qualifiers and Zaki Malik's Lexus LFA Star Showdown are open across Colorado.", 12},
      {5, "purple", "PURPLE WRISTBAND", "Purple", 4100, 4000, "PurpleWristbandUnlock", "NewWristbandUnlocked6",
       "",
       "Purple Wristband unlocked! Only the elite reach this tier — Observatory Street Hub and Hailey Harper await.", 14},
      {6, "gold", "GOLD WRISTBAND", "Gold", 6400, 5000, "GoldWristbandUnlock", "NewWristbandUnlocked7",
       "",
       "Gold Wristband unlocked! You have earned your place among Colorado's best — the Horizon Finale and Darius Flynt await!", 16},
  };
  return kBands;
}

const WristbandMilestoneSpec* find_wristband_milestone(int level) {
  const auto& bands = wristband_milestones();
  for (const auto& b : bands) {
    if (b.level == level) return &b;
  }
  return nullptr;
}

// ---- All 7 Star Showdowns (Exact gamedb.slt Events & EventParticipants) ----

const std::vector<BossRivalSpec>& boss_rivals() {
  static const std::vector<BossRivalSpec> kBosses = {
      {0, 0, 79, "NEM_TEST", "ramona", CharacterId::kRamonaCravache, "RAMONA CRAVACHE", "YELLOW WRISTBAND STAR",
       1460, "FOR_SVTRaptor_12", "2011 Ford F-150 SVT Raptor", 220, 5000, "NemesisEventUnlocked1", "NemesisPreRace1",
       "NemesisPostRaceWin1",
       "Not bad for a rookie, but you're in my dust now. Beat my SVT Raptor head-to-head and the keys are yours.",
       "You actually kept up on the dirt — fair play. Take the Raptor keys, you earned your spot at Horizon!", false},
      {1, 1, 241, "NR02", "ali", CharacterId::kAliHoward, "ALI HOWARD", "GREEN WRISTBAND STAR",
       1557, "DOD_ChallengerSRT8392_12", "2012 Dodge Challenger SRT8 392", 750, 10000, "NemesisEventUnlocked2",
       "NemesisPreRace2", "NemesisPostRaceWin2",
       "Everyone's talking about the newcomer. Let's see if you can handle raw American muscle on the open highway.",
       "Clean pass at the line! The Challenger SRT8 392 is all yours — see you further up the standings.", false},
      {2, 2, 242, "NR03", "duke", CharacterId::kDukeMaguire, "DUKE MAGUIRE", "BLUE WRISTBAND STAR",
       325, "LAM_DiabloSV_97", "1997 Lamborghini Diablo SV", 1450, 15000, "NemesisEventUnlocked3", "NemesisPreRace3",
       "NemesisPostRaceWin3",
       "You want my Diablo SV? You'll have to survive the canyon switchbacks without lifting off the throttle.",
       "Unbelievable drive! Keep the Diablo SV — looks like Darius has a real challenger this year.", false},
      {3, 3, 243, "NR04", "marko", CharacterId::kMarkoBaran, "MARKO BARAN", "PINK WRISTBAND STAR",
       1558, "NIS_GTR_12", "2012 Nissan GT-R Black Edition", 2500, 20000, "NemesisEventUnlocked4", "NemesisPreRace4",
       "NemesisPostRaceWin4",
       "Precision beats hype every time. My GT-R Black Edition owns Red Rock — show me what you've got.",
       "Flawless lines through every apex. The GT-R Black Edition belongs in your garage.", false},
      {4, 4, 244, "NR05", "zaki", CharacterId::kZakiMalik, "ZAKI MALIK", "ORANGE WRISTBAND STAR",
       1260, "LEX_LFA_10", "2010 Lexus LFA", 4100, 30000, "NemesisEventUnlocked5", "NemesisPreRace5",
       "NemesisPostRaceWin5",
       "Listen to that V10 scream across Colorado. If you can catch the LFA before the dam, you win the pink slip.",
       "You outran the LFA — respect! Take the keys and don't let Hailey or Darius intimidate you.", false},
      {5, 5, 245, "NR06", "hailey", CharacterId::kHaileyHarper, "HAILEY HARPER", "PURPLE WRISTBAND STAR",
       432, "SAL_S7_04", "2004 Saleen S7", 6400, 40000, "NemesisEventUnlocked6", "NemesisPreRace6",
       "NemesisPostRaceWin6",
       "One showdown stands between you and the Gold Wristband elite. Catch my Saleen S7 if you can!",
       "Incredible pace! The Saleen S7 is yours — go take the Horizon crown straight from Darius Flynt!", false},
      {6, 6, 169, "NEM_FINAL", "darius", CharacterId::kDariusFlynt, "DARIUS FLYNT", "REIGNING HORIZON CHAMPION",
       1171, "FER_599XX_10", "2010 Ferrari 599XX", 10000, 50000, "NemesisEventUnlocked7", "NemesisPreRace7",
       "NemesisPostRaceWin7",
       "So you won the Horizon Finale? One more run — champion against champion. Beat my Ferrari 599XX for the title and the keys!",
       "I didn't think anyone in Colorado could take my 599XX. The Festival is yours — hail the new Horizon Champion!", true},
  };
  return kBosses;
}

const BossRivalSpec* find_boss_by_event(std::string_view horizon_id) {
  const std::string want = upper_ascii(horizon_id);
  for (const auto& b : boss_rivals()) {
    if (upper_ascii(b.horizon_id) == want) return &b;
  }
  return nullptr;
}

const BossRivalSpec* find_boss_by_key(std::string_view key) {
  std::string want = lower_ascii(key);
  if (want == "haley") want = "hailey";
  for (const auto& b : boss_rivals()) {
    if (want == b.key || want == lower_ascii(b.horizon_id)) return &b;
  }
  return nullptr;
}

const BossRivalSpec* find_boss_by_level(int wristband_level) {
  for (const auto& b : boss_rivals()) {
    if (b.wristband_level == wristband_level) return &b;
  }
  return nullptr;
}

// ---- All 10 Showcases (Exact gamedb.slt Popularity Gates 250 -> 1 & airborne_challenges.xml 1:1) ----

const std::vector<ShowcaseMilestoneSpec>& showcase_milestones() {
  static const std::vector<ShowcaseMilestoneSpec> kShowcases = {
      {0, 101, "PLANE_RACE_001", "MUSTANG VS MUSTANG", "P-51 MUSTANG", 250, 404, "FOR_MustangBOSS429_70",
       "1970 Ford Mustang Boss 429", 5000, true,
       "Holly Cruz here! You're invited to the Mustang vs Mustang Showcase — race a vintage P-51 Mustang warbird across Colorado!",
       "Unbelievable finish! You beat the P-51 Mustang to the line — the 1970 Mustang Boss 429 is yours to keep!"},
      {1, 223, "PLANE_RACE_004", "QUATTRO VS BALLOON", "HOT-AIR BALLOON", 225, 633, "AUD_SportQuattro_83",
       "1983 Audi Sport quattro", 6500, true,
       "Popularity Rank 225 unlocked! Take the Group B Audi Sport quattro head-to-head against a Horizon Hot-Air Balloon across the valley!",
       "Group B legend triumphs beneath the balloon! The 1983 Audi Sport quattro has been delivered to your Horizon Garage."},
      {2, 211, "EXHIBITION_FEST_003", "MINI FESTIVAL EXHIBITION", "FESTIVAL PACK", 200, 1566, "AUS_ShowCaseMiniCooperS_65",
       "1965 Mini Cooper S", 8500, false,
       "Rank 200 Showcase unlocked! Jump into a tuned 1965 Mini Cooper S for a three-lap Festival circuit exhibition.",
       "Crowd favourite! You took first in the Mini Cooper S Showcase — check your garage for the prize car!"},
      {3, 168, "PLANE_RACE_002", "CORVETTE VS HELICOPTER", "HELICOPTER", 175, 1527, "CHE_CORVETTEGSC_10",
       "2010 Chevrolet Corvette Grand Sport", 10000, true,
       "Rank 175 Showcase! Drop the top on a Corvette Grand Sport and outrun the Horizon Helicopter through the canyon!",
       "What a photo finish under the rotor wash! The Corvette Grand Sport Convertible is now in your garage."},
      {4, 246, "PLANE_RACE_007", "F40 VS BIPLANE", "BIPLANE", 150, 340, "FER_F40_87",
       "1987 Ferrari F40", 12500, true,
       "Rank 150 Showcase unlocked! Twin-turbo legend meets wings — drive the 1987 Ferrari F40 against a barnstorming Biplane!",
       "Pure Ferrari magic! You beat the Biplane — the 1987 Ferrari F40 is yours and the fans are going wild!"},
      {5, 214, "EXHIBITION_RESE_003", "RESERVOIR RUN", "EXHIBITION FIELD", 125, 1115, "FIA_500AbarthSS_10",
       "2010 Abarth 500 esseesse", 15000, false,
       "Rank 125 Showcase! Carve the dam twisties in an Abarth 500 esseesse against the Festival exhibition grid.",
       "Pocket rocket victory! The Abarth 500 esseesse is waiting in your Horizon Garage."},
      {6, 172, "PLANE_RACE_003", "AVENTADOR VS BIPLANE", "BIPLANE", 100, 1398, "LAM_LP7004_12",
       "2012 Lamborghini Aventador LP700-4", 17500, true,
       "Top 100 Popularity! Take a Lamborghini Aventador LP700-4 flat out against a low-flying Horizon Biplane!",
       "V12 thunder beats wings across Colorado! The Lamborghini Aventador LP700-4 is yours!"},
      {7, 222, "PLANE_RACE_006", "RGT-8 VS MUSTANG", "P-51 MUSTANG", 75, 1432, "RUF_RGT8_11",
       "2011 RUF RGT-8", 20000, true,
       "Rank 75 Showcase unlocked! Push the 2011 RUF RGT-8 through Colorado's mountain roads against a P-51 Mustang!",
       "Not even the P-51 Mustang could match your pace! The 2011 RUF RGT-8 has been added to your garage."},
      {8, 59, "EXHIBITION_FEST_004", "GROUP B SHOWDOWN", "RALLY LEGENDS", 50, 1533, "PEU_205T16_84",
       "1984 Peugeot 205 T16", 22500, false,
       "Top 50 Popularity! Master the gravel and tarmac in the iconic 1984 Peugeot 205 Turbo 16 Showcase!",
       "Unstoppable! The 1984 Peugeot 205 T16 is now in your Horizon Garage."},
      {9, 220, "PLANE_RACE_005", "ONE-77 VS HELICOPTER", "HELICOPTER", 1, 1181, "AST_One77_10",
       "2010 Aston Martin One-77", 25000, true,
       "You hit #1 in Popularity across the entire Festival! Celebrate in the Aston Martin One-77 against the Horizon Helicopter!",
       "Festival icon! You beat the Helicopter in the #1 Popularity Showcase — the Aston Martin One-77 is yours forever!"},
  };
  return kShowcases;
}

const ShowcaseMilestoneSpec* find_showcase_by_event(std::string_view horizon_id) {
  const std::string want = upper_ascii(horizon_id);
  for (const auto& sc : showcase_milestones()) {
    if (upper_ascii(sc.horizon_id) == want) return &sc;
  }
  return nullptr;
}

void set_campaign_text_resolver(std::function<std::string(std::string_view)> resolver) {
  text_resolver_slot() = std::move(resolver);
}

std::string resolve_event_display_title(std::string_view raw_id_or_title, std::string_view localized_override) {
  auto clean_candidate = [](std::string_view candidate) -> std::string {
    const std::string trimmed = trim_ascii(candidate);
    if (trimmed.empty() || trimmed.rfind("_&", 0) == 0) return {};
    if (looks_like_internal_event_key(trimmed)) return {};
    return trimmed;
  };

  if (std::string loc = clean_candidate(localized_override); !loc.empty()) return loc;

  const std::string trimmed = trim_ascii(raw_id_or_title);
  if (trimmed.empty()) return "HORIZON EVENT";

  // If raw_id_or_title is a string-table reference (`_&...`), resolve it via the bound callback first.
  if (trimmed.rfind("_&", 0) == 0) {
    if (const auto& fn = text_resolver_slot()) {
      if (std::string loc = clean_candidate(fn(trimmed)); !loc.empty()) return loc;
    }
    return "HORIZON EVENT";
  }

  const std::string up = upper_ascii(trimmed);
  if (const ShowcaseMilestoneSpec* sc = find_showcase_by_event(up)) {
    return sc->display_name;
  }
  if (const BossRivalSpec* boss = find_boss_by_event(up)) {
    if (boss->is_finale_champion) return "CHAMPION VS CHAMPION: DARIUS FLYNT";
    return std::string("STAR SHOWDOWN: ") + boss->rival_name;
  }
  if (up == "FESTIVAL_FIRST") return "HORIZON HEATS";
  if (up == "FESTIVAL_SECOND" || up == "FESTIVAL_INTRO") return "HORIZON QUALIFIER";
  if (up == "HEADLINE_EVENT") return "HORIZON FESTIVAL FINALE";
  if (up == "STREET_INTRO") return "STREET RACE QUALIFIER";

  // Already a clean human-readable title (not an internal key).
  if (!looks_like_internal_event_key(trimmed)) return trimmed;

  // Short coded keys (FR05, SR03, NR02, ST01, PR04).
  {
    size_t alpha_n = 0;
    while (alpha_n < up.size() && std::isalpha(static_cast<unsigned char>(up[alpha_n]))) ++alpha_n;
    size_t digit_n = 0;
    while (alpha_n + digit_n < up.size() && std::isdigit(static_cast<unsigned char>(up[alpha_n + digit_n]))) ++digit_n;
    if (alpha_n >= 2 && alpha_n <= 3 && digit_n >= 1 && alpha_n + digit_n == up.size()) {
      const std::string pfx = up.substr(0, alpha_n);
      const std::string num = up.substr(alpha_n);
      if (pfx == "FR") return "FESTIVAL RACE " + num;
      if (pfx == "SR") return "STREET RACE " + num;
      if (pfx == "NR") return "STAR SHOWDOWN " + num;
      if (pfx == "ST") return "STREET TOUR " + num;
      if (pfx == "PR") return "PR STUNT " + num;
      if (pfx == "EV") return "HORIZON EVENT " + num;
    }
  }

  // Generic snake_case internal key prettification (e.g. RED_ROCK_RUN_001 -> RED ROCK RUN).
  std::vector<std::string> tokens;
  size_t pos = 0;
  while (pos < up.size()) {
    while (pos < up.size() && up[pos] == '_') ++pos;
    size_t end = up.find('_', pos);
    if (end == std::string::npos) end = up.size();
    if (end > pos) tokens.push_back(up.substr(pos, end - pos));
    pos = end;
  }
  if (tokens.size() >= 2) {
    const auto& last = tokens.back();
    bool all_digits = !last.empty();
    for (char c : last) {
      if (!std::isdigit(static_cast<unsigned char>(c))) {
        all_digits = false;
        break;
      }
    }
    if (all_digits) {
      const int n = std::atoi(last.c_str());
      if (n <= 1 && tokens.size() >= 3) {
        tokens.pop_back();
      } else {
        tokens.back() = std::to_string(n);
      }
    }
  }
  std::string pretty;
  for (size_t i = 0; i < tokens.size(); ++i) {
    if (i > 0) pretty.push_back(' ');
    pretty += tokens[i];
  }
  return pretty.empty() ? std::string("HORIZON EVENT") : pretty;
}

std::string resolve_event_display_title(const CareerEvent* ev, std::string_view localized_override) {
  if (!ev) return resolve_event_display_title("", localized_override);
  if (!localized_override.empty()) {
    std::string loc = resolve_event_display_title(ev->horizon_id, localized_override);
    if (!loc.empty()) return loc;
  }
  if (!ev->name_ref.empty()) {
    if (const auto& fn = text_resolver_slot()) {
      const std::string resolved = fn(ev->name_ref);
      if (!resolved.empty() && resolved != ev->name_ref && resolved.rfind("_&", 0) != 0 &&
          !looks_like_internal_event_key(resolved)) {
        return trim_ascii(resolved);
      }
    }
  }
  return resolve_event_display_title(ev->horizon_id, {});
}

const char* cutscene_id_name(CutsceneKind kind) {
  switch (kind) {
    case CutsceneKind::kNone: return "none";
    case CutsceneKind::kOpeningFmv: return "fmv_01_opening";
    case CutsceneKind::kWristbandCeremonyFmv: return "fmv_02_wristband";
    case CutsceneKind::kFinaleCreditsFmv: return "fmv_04_credits";
    case CutsceneKind::kWristbandStageCinematic: return "cin_wristband_stage";
    case CutsceneKind::kShowcaseIntroCinematic: return "cin_showcase_intro";
    case CutsceneKind::kShowcaseOutroCinematic: return "cin_showcase_outro";
    case CutsceneKind::kBossIntroCinematic: return "cin_boss_intro";
    case CutsceneKind::kBossVictoryCinematic: return "cin_boss_victory";
    case CutsceneKind::kBarnDiscoveryCinematic: return "cin_barn_discovery";
    case CutsceneKind::kBarnRestoredCinematic: return "cin_barn_restored";
    case CutsceneKind::kPreRaceGridFlyby: return "cin_prerace_grid";
    case CutsceneKind::kPostRaceFinishCam: return "cin_postrace_finish";
    case CutsceneKind::kFinaleVictoryCinematic: return "cin_finale_victory";
  }
  return "none";
}

// ---- DialogQueue ----

bool DialogQueue::enqueue(const DialogCue& cue, std::set<std::string>* heard_set) {
  if (cue.subtitle.empty() && cue.vo_trigger.empty() && cue.radio_event.empty()) return false;
  if (cue.one_shot && heard_set && !cue.id.empty() && heard_set->count(cue.id) > 0) return false;
  if (cue.one_shot && heard_set && !cue.id.empty()) heard_set->insert(cue.id);

  if (cue.interrupt) {
    active_ = false;
    queue_.clear();
  }
  if (queue_.size() >= 12) queue_.erase(queue_.begin());
  queue_.push_back(cue);
  if (!active_) begin_next();
  return true;
}

void DialogQueue::set_vo_telemetry(const DialogVoTelemetry& vo) {
  set_vo_telemetry(vo.vo_playing, vo.vo_elapsed_s, vo.vo_duration_s, vo.vo_rms, vo.active_trigger);
}

void DialogQueue::set_vo_telemetry(bool vo_playing, float vo_elapsed_s, float vo_duration_s, float vo_rms,
                                   std::string_view active_trigger) {
  has_vo_telemetry_ = true;
  vo_playing_ = vo_playing;
  vo_elapsed_s_ = std::max(0.0f, vo_elapsed_s);
  vo_duration_s_ = std::max(0.0f, vo_duration_s);
  vo_rms_ = std::isfinite(vo_rms) ? std::max(0.0f, vo_rms) : 0.0f;
  vo_active_trigger_.assign(active_trigger.begin(), active_trigger.end());
}

void DialogQueue::begin_next() {
  if (queue_.empty()) {
    active_ = false;
    elapsed_ = 0.0;
    effective_duration_s_ = 0.0;
    audio_pending_ = false;
    vo_seen_playing_ = false;
    vo_hold_active_ = false;
    vo_ended_elapsed_ = -1.0;
    return;
  }
  // Pick highest priority cue (stable FIFO within equal priority).
  size_t best = 0;
  for (size_t i = 1; i < queue_.size(); ++i) {
    if (queue_[i].priority > queue_[best].priority) best = i;
  }
  current_ = std::move(queue_[best]);
  queue_.erase(queue_.begin() + ptrdiff_t(best));
  if (current_.duration_s < 1.5) current_.duration_s = 4.5;
  active_ = true;
  elapsed_ = 0.0;
  effective_duration_s_ = current_.duration_s;
  vo_seen_playing_ = false;
  vo_hold_active_ = false;
  vo_ended_elapsed_ = -1.0;
  audio_pending_ = (!current_.vo_trigger.empty() || !current_.radio_event.empty());
}

void DialogQueue::update(double dt, const DialogVoTelemetry& vo) {
  set_vo_telemetry(vo);
  update(dt);
}

void DialogQueue::update(double dt) {
  dt = std::max(0.0, dt);
  if (active_) {
    elapsed_ += dt;
    bool advance = false;
    if (has_vo_telemetry_ && !current_.vo_trigger.empty()) {
      auto iequals_ascii = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
          if (std::tolower(static_cast<unsigned char>(a[i])) !=
              std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
          }
        }
        return true;
      };
      const bool trigger_match = vo_active_trigger_.empty() || iequals_ascii(vo_active_trigger_, current_.vo_trigger);
      const bool matching_vo_playing = vo_playing_ && trigger_match;
      const double safety_cap_s = (vo_duration_s_ > 0.5f && vo_duration_s_ < 60.0f)
                                      ? std::clamp(double(vo_duration_s_) + 4.0,
                                                   std::max(current_.duration_s, double(vo_duration_s_) + 0.5), 45.0)
                                      : std::clamp(std::max(current_.duration_s * 2.5, 18.0), 12.0, 30.0);

      if (matching_vo_playing) {
        vo_seen_playing_ = true;
        vo_ended_elapsed_ = -1.0;
        if (vo_duration_s_ > 0.5f && vo_duration_s_ < 60.0f) {
          effective_duration_s_ = std::max(1.5, double(vo_duration_s_) + 0.35);
        }
        if (elapsed_ < safety_cap_s) {
          vo_hold_active_ = true;
          if (elapsed_ + 0.4 > effective_duration_s_) {
            effective_duration_s_ = std::min(safety_cap_s, elapsed_ + 0.5);
          }
        } else {
          vo_hold_active_ = false;
          advance = true;
        }
      } else {
        vo_hold_active_ = false;
        if (vo_seen_playing_) {
          if (vo_ended_elapsed_ < 0.0) vo_ended_elapsed_ = elapsed_;
          const double finish_at = std::max(1.5, vo_ended_elapsed_ + 0.35);
          effective_duration_s_ = std::min(safety_cap_s, finish_at);
          if (elapsed_ >= finish_at || elapsed_ >= safety_cap_s) advance = true;
        } else {
          // Missing/failed VO clip: fall back cleanly to static subtitle duration_s.
          effective_duration_s_ = current_.duration_s;
          if (elapsed_ >= current_.duration_s) advance = true;
        }
      }
    } else {
      vo_hold_active_ = false;
      effective_duration_s_ = current_.duration_s;
      if (elapsed_ >= current_.duration_s) advance = true;
    }
    if (advance) {
      begin_next();
    }
  }
  const float target_duck = active_ ? 1.0f : 0.0f;
  const float rate = active_ ? 7.0f : 3.5f;  // fast attack duck, smooth release
  duck_ += (target_duck - duck_) * float(std::min(1.0, dt * rate));
  if (std::fabs(duck_ - target_duck) < 0.002f) duck_ = target_duck;
}

void DialogQueue::skip_current() {
  if (!active_) return;
  begin_next();
}

void DialogQueue::clear() {
  active_ = false;
  queue_.clear();
  elapsed_ = 0.0;
  effective_duration_s_ = 0.0;
  audio_pending_ = false;
  vo_seen_playing_ = false;
  vo_hold_active_ = false;
  vo_ended_elapsed_ = -1.0;
}

float DialogQueue::banner_slide() const {
  if (!active_) return 0.0f;
  constexpr double kIn = 0.35;
  constexpr double kOut = 0.35;
  if (elapsed_ < kIn) return float(ease_out_cubic(elapsed_ / kIn));
  if (vo_hold_active_) return 1.0f;
  const double dur = effective_duration_s_ > 1e-4 ? effective_duration_s_ : current_.duration_s;
  const double rem = dur - elapsed_;
  if (rem < kOut) return float(ease_out_cubic(std::max(0.0, rem) / kOut));
  return 1.0f;
}

std::vector<float> DialogQueue::waveform_bars(int count) const {
  count = std::clamp(count, 4, 24);
  std::vector<float> bars(size_t(count), 0.12f);
  if (!active_) return bars;
  const double t = elapsed_;
  const double env = double(banner_slide());
  if (vo_rms_ > 0.001f) {
    // Live speech amplitude from VoPlayer block RMS drives the call-in visualizer.
    const double level = std::clamp(std::pow(double(vo_rms_) / 0.30, 0.65), 0.0, 1.0);
    const double center = 0.5 * double(count - 1);
    for (int i = 0; i < count; ++i) {
      const double norm_dist = (count > 1) ? std::fabs(double(i) - center) / std::max(1.0, center) : 0.0;
      const double arch = 1.0 - 0.35 * norm_dist * norm_dist;
      const double shimmer = 0.72 + 0.28 * std::fabs(std::sin(t * (7.5 + double(i % 4) * 2.3) + double(i) * 1.17));
      const double raw = (0.10 + 0.90 * level * arch * shimmer) * env;
      bars[size_t(i)] = float(std::clamp(raw, 0.10, 1.0));
    }
    return bars;
  }
  if (has_vo_telemetry_ && vo_playing_) {
    for (int i = 0; i < count; ++i) {
      bars[size_t(i)] = float(std::clamp((0.10 + 0.04 * std::fabs(std::sin(t * 4.0 + double(i)))) * env, 0.10, 0.16));
    }
    return bars;
  }
  for (int i = 0; i < count; ++i) {
    const double phase = t * (6.5 + double(i % 5) * 2.1) + double(i) * 0.93;
    const double voice_mod = 0.5 + 0.5 * std::sin(t * 3.7 + double(i) * 0.45);
    const double raw = (0.28 + 0.72 * std::fabs(std::sin(phase)) * voice_mod) * env;
    bars[size_t(i)] = float(std::clamp(raw, 0.10, 1.0));
  }
  return bars;
}

bool DialogQueue::consume_pending_audio(std::string& out_vo_trigger, std::string& out_radio_event) {
  if (!audio_pending_ || !active_) return false;
  audio_pending_ = false;
  out_vo_trigger = current_.vo_trigger;
  out_radio_event = current_.radio_event;
  return true;
}

// ---- TransitionState ----

void TransitionState::start(TransitionKind k, double duration, std::string text) {
  kind = k;
  duration_s = std::max(0.12, duration);
  elapsed_s = 0.0;
  active = (k != TransitionKind::kNone);
  midpoint_fired = false;
  peak = 0.5;
  label = std::move(text);
}

void TransitionState::start_split(TransitionKind k, double fade_out, double fade_in, std::string text) {
  fade_out = std::max(0.0, fade_out);
  fade_in = std::max(0.0, fade_in);
  start(k, fade_out + fade_in, std::move(text));
  if (fade_out + fade_in > 1e-6) peak = std::clamp(fade_out / (fade_out + fade_in), 0.05, 0.95);
}

bool TransitionState::update(double dt) {
  if (!active) return false;
  elapsed_s += std::max(0.0, dt);
  bool crossed = false;
  if (!midpoint_fired && elapsed_s >= duration_s * peak) {
    midpoint_fired = true;
    crossed = true;
  }
  if (elapsed_s >= duration_s) {
    active = false;
    elapsed_s = duration_s;
  }
  return crossed;
}

double TransitionState::progress() const {
  if (!active || duration_s <= 1e-4) return 0.0;
  return std::clamp(elapsed_s / duration_s, 0.0, 1.0);
}

float TransitionState::cover_alpha() const {
  if (!active) return 0.0f;
  const double p = progress();
  if (p <= peak) return float(ease_in_out_cubic(p / peak));
  return float(ease_in_out_cubic((1.0 - p) / (1.0 - peak)));
}

float TransitionState::wipe_edge() const {
  if (!active) return 0.0f;
  return float(ease_in_out_cubic(progress()));
}

// ---- RewardTallyState ----

double RewardTallyState::stage_duration_s() const {
  switch (stage) {
    case PostRaceStage::kRaceResults: return 1.2;
    case PostRaceStage::kCreditsEarned: return 2.25;
    case PostRaceStage::kWristbandAndPrize: return 1.85;
  }
  return 2.0;
}

void RewardTallyState::update(double dt) {
  if (!active) return;
  const double step = std::max(0.0, dt);
  elapsed_s = std::min(duration_s + 10.0, elapsed_s + step);
  stage_elapsed_s = std::min(stage_duration_s() + 10.0, stage_elapsed_s + step);
}

void RewardTallyState::skip_to_end() {
  if (!active) return;
  stage_elapsed_s = std::max(stage_elapsed_s, stage_duration_s());
  if (is_final_stage()) {
    elapsed_s = std::max(elapsed_s, duration_s);
  }
}

bool RewardTallyState::advance_stage() {
  if (!active) return false;
  if (stage == PostRaceStage::kRaceResults) {
    stage = PostRaceStage::kCreditsEarned;
    stage_elapsed_s = 0.0;
    return true;
  }
  if (stage == PostRaceStage::kCreditsEarned) {
    stage = PostRaceStage::kWristbandAndPrize;
    stage_elapsed_s = 0.0;
    return true;
  }
  return false;
}

int64_t RewardTallyState::displayed_credits() const {
  if (stage == PostRaceStage::kWristbandAndPrize) return total_credits;
  const double t = (stage == PostRaceStage::kCreditsEarned) ? stage_elapsed_s : elapsed_s;
  const double p = ease_out_cubic(std::clamp((t - 0.25) / 1.80, 0.0, 1.0));
  return int64_t(std::llround(double(total_credits) * p));
}

int64_t RewardTallyState::displayed_balance() const {
  return balance_before + displayed_credits();
}

int RewardTallyState::displayed_pop_rank() const {
  const double t = (stage == PostRaceStage::kWristbandAndPrize) ? stage_elapsed_s : elapsed_s;
  const double p = (stage == PostRaceStage::kWristbandAndPrize)
                       ? ease_out_cubic(std::clamp((t - 0.25) / 1.35, 0.0, 1.0))
                       : ease_out_cubic(std::clamp((t - 1.1) / 1.4, 0.0, 1.0));
  const double r = double(pop_rank_before) + double(pop_rank_after - pop_rank_before) * p;
  return int(std::lround(r));
}

int RewardTallyState::displayed_wb_points() const {
  const double t = (stage == PostRaceStage::kWristbandAndPrize) ? stage_elapsed_s : elapsed_s;
  const double p = (stage == PostRaceStage::kWristbandAndPrize)
                       ? ease_out_cubic(std::clamp((t - 0.20) / 1.45, 0.0, 1.0))
                       : ease_out_cubic(std::clamp((t - 1.6) / 1.6, 0.0, 1.0));
  return wb_before + int(std::lround(double(wb_after - wb_before) * p));
}

float RewardTallyState::wb_bar_fraction() const {
  if (wb_tier_hi <= wb_tier_lo) return 1.0f;
  const double span = double(wb_tier_hi - wb_tier_lo);
  const double cur = double(displayed_wb_points() - wb_tier_lo);
  return float(std::clamp(cur / span, 0.0, 1.0));
}

float RewardTallyState::tier_up_flash() const {
  if (!wb_tier_up || !active) return 0.0f;
  const double t_ref = (stage == PostRaceStage::kWristbandAndPrize) ? (stage_elapsed_s - 1.25) : (elapsed_s - 2.6);
  if (t_ref < 0.0) return 0.0f;
  return float(0.5 + 0.5 * std::sin(t_ref * 7.5)) * float(std::clamp(1.0 - t_ref * 0.12, 0.35, 1.0));
}

float RewardTallyState::row_reveal(int row_idx) const {
  if (stage == PostRaceStage::kWristbandAndPrize) return 1.0f;
  const double t = (stage == PostRaceStage::kCreditsEarned) ? stage_elapsed_s : elapsed_s;
  const double start = 0.15 + double(std::max(0, row_idx)) * 0.16;
  return float(ease_out_cubic(std::clamp((t - start) / 0.32, 0.0, 1.0)));
}

// ---- CampaignFlow ----

bool CampaignFlow::is_onboarding() const {
  return stage_ == CampaignStage::kOpeningCutscene || stage_ == CampaignStage::kViperIntroDrive ||
         stage_ == CampaignStage::kQualifyRace || stage_ == CampaignStage::kFestivalRegistration;
}

bool CampaignFlow::is_cinematic_active() const {
  if (stage_ == CampaignStage::kQualifyRace && qualify_retry_pending_) return true;
  switch (stage_) {
    case CampaignStage::kOpeningCutscene:
    case CampaignStage::kFestivalRegistration:
    case CampaignStage::kWristbandCeremony:
    case CampaignStage::kShowcaseIntro:
    case CampaignStage::kShowcaseOutro:
    case CampaignStage::kBossIntro:
    case CampaignStage::kBossVictory:
    case CampaignStage::kBarnDiscovery:
    case CampaignStage::kBarnRestored:
    case CampaignStage::kPreRaceGrid:
    case CampaignStage::kPostRaceFinish:
    case CampaignStage::kFinaleIntro:
    case CampaignStage::kFinaleVictory:
    case CampaignStage::kCredits:
      return true;
    default:
      return false;
  }
}

bool CampaignFlow::is_blocking_gameplay() const {
  if (stage_ == CampaignStage::kQualifyRace && qualify_retry_pending_) return true;
  return stage_ == CampaignStage::kOpeningCutscene || stage_ == CampaignStage::kFestivalRegistration ||
         stage_ == CampaignStage::kWristbandCeremony || stage_ == CampaignStage::kShowcaseIntro ||
         stage_ == CampaignStage::kShowcaseOutro || stage_ == CampaignStage::kBossIntro ||
         stage_ == CampaignStage::kBossVictory || stage_ == CampaignStage::kBarnDiscovery ||
         stage_ == CampaignStage::kBarnRestored || stage_ == CampaignStage::kPreRaceGrid ||
         stage_ == CampaignStage::kPostRaceFinish || stage_ == CampaignStage::kRewardTally ||
         stage_ == CampaignStage::kFinaleIntro || stage_ == CampaignStage::kFinaleVictory ||
         stage_ == CampaignStage::kCredits;
}

float CampaignFlow::hud_reveal_alpha() const {
  if (stage_ != CampaignStage::kViperIntroDrive) return 1.0f;
  return float(ease_out_cubic(std::clamp((stage_time_ - 0.6) / 1.8, 0.0, 1.0)));
}

int CampaignFlow::credits_scroll_line_count() const { return 36; }

void CampaignFlow::mark_cutscene_done(CutsceneKind kind) {
  if (kind == CutsceneKind::kNone) return;
  completed_cutscenes_.insert(cutscene_id_name(kind));
}

CampaignAction CampaignFlow::start_new_career(bool skip_movies, std::array<double, 3> festival_gate) {
  skip_movies_ = skip_movies;
  festival_gate_ = festival_gate;
  qualify_launched_ = false;
  qualify_retry_pending_ = false;
  viper_beat_idx_ = 0;
  viper_convoy_passed_ = 0;
  reg_beat_idx_ = 0;
  active_wristband_ = 0;
  active_boss_idx_ = -1;
  active_showcase_idx_ = -1;
  finale_complete_ = false;
  pending_finale_credits_ = false;
  pending_darius_showdown_ = false;
  starter_autoshow_onboarding_ = false;
  completed_cutscenes_.clear();
  heard_dialogs_.clear();
  unlocked_bosses_.clear();
  unlocked_showcases_.clear();
  dialogs_.clear();
  tally_ = RewardTallyState{};

  if (!skip_movies_) {
    return enter_stage(CampaignStage::kOpeningCutscene, TransitionKind::kCrossFade);
  }
  return enter_stage(CampaignStage::kViperIntroDrive, TransitionKind::kBrushWipe);
}

CampaignAction CampaignFlow::enter_stage(CampaignStage next, TransitionKind trans) {
  stage_ = next;
  stage_time_ = 0.0;
  if (trans != TransitionKind::kNone) {
    transition_.start(trans, 0.55, next == CampaignStage::kViperIntroDrive ? "HORIZON COLORADO" : "");
  }

  CampaignAction act;
  cinematic_ = CinematicBanner{};
  if (next != CampaignStage::kRewardTally) tally_.active = false;

  switch (next) {
    case CampaignStage::kOpeningCutscene:
      cinematic_.kind = CutsceneKind::kOpeningFmv;
      cinematic_.duration_s = 45.0;
      cinematic_.letterbox_amount = 1.0f;
      cinematic_.eyebrow = "PLAYGROUND GAMES  •  TURN 10 STUDIOS";
      cinematic_.headline = "HORIZON FESTIVAL COLORADO";
      cinematic_.subheadline = "OPENING MONTAGE";
      cinematic_.hold_to_skip = true;
      act.play_fmv = true;
      act.fmv_cutscene = CutsceneKind::kOpeningFmv;
      break;

    case CampaignStage::kViperIntroDrive:
      viper_beat_idx_ = 0;
      viper_convoy_passed_ = 0;
      qualify_retry_pending_ = false;
      act.swap_car = true;
      act.car_media = kViperMedia;
      act.car_id = kViperCarId;
      act.teleport = true;
      act.pos = viper_spawn_;
      act.heading = viper_heading_;
      act.set_gps = true;
      act.gps_label = "HORIZON FESTIVAL MAIN GATE";
      act.gps_xz = {festival_gate_[0], festival_gate_[2]};
      act.lock_viper_radio = true;
      dist_to_festival_ = std::hypot(festival_gate_[0] - viper_spawn_[0], festival_gate_[2] - viper_spawn_[2]);
      act.save_requested = true;
      break;

    case CampaignStage::kQualifyRace:
      qualify_retry_pending_ = false;
      act.swap_car = true;
      act.car_media = kCorradoMedia;
      act.car_id = kCorradoCarId;
      act.launch_event = true;
      act.event_id = kQualifyEventId;
      act.route_id = kQualifyRouteId;
      qualify_launched_ = true;
      dialogs_.enqueue(
          {"m0_corrado_qualify", CharacterId::kAliceHart, "", "", "FirstRaceSignUp", "AliceOpeningInterview2A",
           "Take the 1995 VW Corrado VR6 onto the qualifying grid — finish in the top 3 to claim your Yellow Wristband!",
           6.5, 5, true, true},
          &heard_dialogs_);
      act.save_requested = true;
      break;

    case CampaignStage::kFestivalRegistration:
      reg_beat_idx_ = 0;
      active_wristband_ = 0;
      cinematic_.kind = CutsceneKind::kWristbandStageCinematic;
      cinematic_.duration_s = 14.5;
      cinematic_.letterbox_amount = 1.0f;
      cinematic_.eyebrow = "WELCOME TO HORIZON  •  FIRST FESTIVAL CAR";
      cinematic_.headline = "YELLOW WRISTBAND EARNED";
      cinematic_.subheadline = "1995 VOLKSWAGEN CORRADO VR6 ASSIGNED  •  15,000 CR STARTER BONUS";
      cinematic_.detail_line =
          "3 YELLOW WRISTBAND EVENTS UNLOCKED — KEEP YOUR CORRADO VR6 OR PICK A CURATED AFFORDABLE STARTER RIDE AT THE AUTOSHOW.";
      cinematic_.badge_text = "WRISTBAND TIER 1 / 7  •  WELCOME TO AUTOSHOW";
      cinematic_.colour_key = "Yellow";
      cinematic_.reward_car = "1995 Volkswagen Corrado VR6";
      cinematic_.accent_rgba = pack_rgba(0xFF, 0xD6, 0x00);
      cinematic_.wristband_level = 0;
      act.swap_car = true;
      act.car_media = kCorradoMedia;
      act.car_id = kCorradoCarId;
      act.grant_starter = true;
      act.collect_yellow_wristband = true;
      act.set_gps = true;
      act.gps_label = "RACE CENTRAL  •  FIRST FESTIVAL EVENT";
      act.gps_xz = {festival_gate_[0], festival_gate_[2]};
      if (!skip_movies_) {
        act.play_fmv = true;
        act.fmv_cutscene = CutsceneKind::kWristbandCeremonyFmv;
      }
      dialogs_.enqueue(
          {"m0_alice_welcome", CharacterId::kAliceHart, "", "", "FirstWristbandCollect", "",
           "You made it! Here is your official Yellow Wristband — welcome to the Horizon Festival!",
           5.0, 8, true, true},
          &heard_dialogs_);
      dialogs_.enqueue(
          {"m0_dak_garage", CharacterId::kDakStewart, "", "", "WorkshopFirstTime", "",
           "Name's Dak. That 1995 VW Corrado VR6 is all yours — head into the Autoshow next to check out curated starter rides or drive straight out to Colorado!",
           5.5, 6, true, false},
          &heard_dialogs_);
      dialogs_.enqueue(
          {"m0_holly_events", CharacterId::kHollyCruz, "", "", "FIrstYellowEventOnMap", "",
           "I've set your GPS for your Green Wristband climb. Win Yellow Wristband events, build Popularity, and challenge Ramona Cravache!",
           5.5, 5, true, false},
          &heard_dialogs_);
      act.save_requested = true;
      break;

    case CampaignStage::kFreeRoam:
      act.enter_free_roam = true;
      act.save_requested = true;
      break;

    case CampaignStage::kWristbandCeremony: {
      const WristbandMilestoneSpec* wb = find_wristband_milestone(active_wristband_);
      cinematic_.kind = CutsceneKind::kWristbandStageCinematic;
      cinematic_.duration_s = 7.5;
      cinematic_.letterbox_amount = 1.0f;
      cinematic_.eyebrow = "HORIZON RACE CENTRAL CEREMONY";
      cinematic_.headline = wb ? wb->display_name : "NEW WRISTBAND UNLOCKED";
      cinematic_.subheadline =
          wb ? (std::to_string(wb->unlocked_events_hint) + " NEW FESTIVAL EVENTS  •  STAR SHOWDOWN UNLOCKED")
             : "NEW FESTIVAL EVENTS UNLOCKED";
      cinematic_.detail_line = wb ? wb->announcement : "";
      cinematic_.badge_text = "WRISTBAND TIER " + std::to_string(std::clamp(active_wristband_ + 1, 1, 7)) + " / 7";
      cinematic_.colour_key = wb ? wb->colour_key : "Green";
      cinematic_.wristband_level = active_wristband_;
      cinematic_.accent_rgba = pack_rgba(0xF9, 0x00, 0x64);
      if (wb) {
        if (wb->level == 0) cinematic_.accent_rgba = pack_rgba(0xFF, 0xD6, 0x00);
        else if (wb->level == 1) cinematic_.accent_rgba = pack_rgba(0x46, 0xDC, 0x6E);
        else if (wb->level == 2) cinematic_.accent_rgba = pack_rgba(0x2C, 0x8C, 0xE8);
        else if (wb->level == 3) cinematic_.accent_rgba = pack_rgba(0xF9, 0x00, 0x64);
        else if (wb->level == 4) cinematic_.accent_rgba = pack_rgba(0xFF, 0x7A, 0x00);
        else if (wb->level == 5) cinematic_.accent_rgba = pack_rgba(0xA4, 0x45, 0xF5);
        else if (wb->level == 6) cinematic_.accent_rgba = pack_rgba(0xFF, 0xD2, 0x3F);
      }
      break;
    }

    case CampaignStage::kShowcaseIntro: {
      const auto& scs = showcase_milestones();
      const auto& sc = scs[size_t(std::clamp(active_showcase_idx_, 0, int(scs.size()) - 1))];
      cinematic_.kind = CutsceneKind::kShowcaseIntroCinematic;
      cinematic_.duration_s = 6.0;
      cinematic_.letterbox_amount = 1.0f;
      cinematic_.eyebrow = "HORIZON SHOWCASE EVENT";
      cinematic_.headline = sc.display_name;
      cinematic_.subheadline = std::string(sc.loaner_car_display) + "  VS  " + sc.opponent_label;
      cinematic_.detail_line = "BEAT THE " + std::string(sc.opponent_label) + " TO WIN THE " + sc.loaner_car_display;
      cinematic_.badge_text = "POPULARITY RANK #" + std::to_string(sc.popularity_rank_req) + " SHOWCASE";
      cinematic_.accent_rgba = pack_rgba(0xF9, 0x00, 0x64);
      cinematic_.showcase_index = sc.index;
      dialogs_.enqueue(
          {"sc_intro_" + std::string(sc.horizon_id), CharacterId::kHollyCruz, "", "", "FirstShowcaseEvent", "",
           sc.intro_line, 5.5, 7, false, true});
      break;
    }

    case CampaignStage::kShowcaseOutro: {
      const auto& scs = showcase_milestones();
      const auto& sc = scs[size_t(std::clamp(active_showcase_idx_, 0, int(scs.size()) - 1))];
      cinematic_.kind = CutsceneKind::kShowcaseOutroCinematic;
      cinematic_.duration_s = 6.5;
      cinematic_.letterbox_amount = 1.0f;
      cinematic_.eyebrow = "SHOWCASE VICTORY!";
      cinematic_.headline = std::string(sc.display_name) + " COMPLETE";
      cinematic_.subheadline = "NEW PRIZE CAR DELIVERED TO YOUR GARAGE";
      cinematic_.reward_car = sc.loaner_car_display;
      cinematic_.detail_line = sc.victory_line;
      cinematic_.badge_text = "SHOWCASE WINNER";
      cinematic_.accent_rgba = pack_rgba(0xDA, 0xFF, 0x00);
      cinematic_.showcase_index = sc.index;
      dialogs_.enqueue(
          {"sc_win_" + std::string(sc.horizon_id), CharacterId::kAliceHart, "", "", "FirstShowcaseWin", "",
           sc.victory_line, 5.5, 7, false, true});
      break;
    }

    case CampaignStage::kBossIntro:
    case CampaignStage::kFinaleIntro: {
      const auto& bosses = boss_rivals();
      const int idx = next == CampaignStage::kFinaleIntro
                          ? int(bosses.size()) - 1
                          : std::clamp(active_boss_idx_, 0, int(bosses.size()) - 1);
      active_boss_idx_ = idx;
      const auto& boss = bosses[size_t(idx)];
      cinematic_.kind = CutsceneKind::kBossIntroCinematic;
      cinematic_.duration_s = 6.8;
      cinematic_.letterbox_amount = 1.0f;
      cinematic_.eyebrow = boss.is_finale_champion ? "HORIZON FESTIVAL FINALE  •  CHAMPION VS CHAMPION"
                                                   : "HORIZON STAR SHOWDOWN  •  1-ON-1 PINK SLIP";
      cinematic_.headline = boss.rival_name;
      cinematic_.subheadline = std::string(boss.rival_title) + "  •  " + boss.car_display;
      cinematic_.detail_line = "BEAT " + std::string(boss.rival_name) + " TO WIN THE " + boss.car_display + " PINK SLIP";
      cinematic_.badge_text = boss.is_finale_champion ? "FINAL BOSS SHOWDOWN" : "STAR SHOWDOWN #" + std::to_string(boss.index + 1);
      cinematic_.accent_rgba = character_profile(boss.character).accent_rgba;
      cinematic_.boss_index = boss.index;
      cinematic_.wristband_level = boss.wristband_level;
      if (const auto* wb = find_wristband_milestone(boss.wristband_level)) cinematic_.colour_key = wb->colour_key;
      dialogs_.enqueue(
          {"boss_pre_" + std::string(boss.key), boss.character, "", "", boss.vo_pre_race, "", boss.challenge_line,
           6.0, 9, false, true});
      break;
    }

    case CampaignStage::kBossVictory: {
      const auto& bosses = boss_rivals();
      const auto& boss = bosses[size_t(std::clamp(active_boss_idx_, 0, int(bosses.size()) - 1))];
      cinematic_.kind = CutsceneKind::kBossVictoryCinematic;
      cinematic_.duration_s = 6.8;
      cinematic_.letterbox_amount = 1.0f;
      cinematic_.eyebrow = "STAR SHOWDOWN VICTORY  •  PINK SLIP WON";
      cinematic_.headline = std::string(boss.rival_name) + " DEFEATED";
      cinematic_.subheadline = "RIVAL VEHICLE ADDED TO YOUR HORIZON GARAGE";
      cinematic_.reward_car = boss.car_display;
      cinematic_.detail_line = boss.victory_line;
      cinematic_.badge_text = "PINK SLIP CLAIMED";
      cinematic_.accent_rgba = pack_rgba(0xDA, 0xFF, 0x00);
      cinematic_.boss_index = boss.index;
      dialogs_.enqueue(
          {"boss_win_" + std::string(boss.key), boss.character, "", "", boss.vo_post_win, "", boss.victory_line,
           6.0, 9, false, true});
      break;
    }

    case CampaignStage::kBarnDiscovery:
      cinematic_.kind = CutsceneKind::kBarnDiscoveryCinematic;
      cinematic_.duration_s = 6.2;
      cinematic_.letterbox_amount = 1.0f;
      cinematic_.eyebrow = "COLORADO BARN FIND DISCOVERED";
      cinematic_.headline = "HIDDEN CLASSIC UNCOVERED";
      cinematic_.subheadline = "DAK STEWART IS TOWING IT BACK TO THE HORIZON GARAGE";
      cinematic_.badge_text = "BARN FIND";
      cinematic_.accent_rgba = pack_rgba(0xFF, 0xB8, 0x1C);
      break;

    case CampaignStage::kBarnRestored:
      cinematic_.kind = CutsceneKind::kBarnRestoredCinematic;
      cinematic_.duration_s = 6.2;
      cinematic_.letterbox_amount = 1.0f;
      cinematic_.eyebrow = "DAK'S GARAGE RESTORATION COMPLETE";
      cinematic_.headline = "BARN FIND READY TO DRIVE";
      cinematic_.subheadline = "RESTORED TO CONCOURS CONDITION & DELIVERED TO YOUR GARAGE";
      cinematic_.badge_text = "RESTORATION COMPLETE";
      cinematic_.accent_rgba = pack_rgba(0x46, 0xDC, 0x6E);
      break;

    case CampaignStage::kPreRaceGrid:
      cinematic_.kind = CutsceneKind::kPreRaceGridFlyby;
      cinematic_.duration_s = 4.2;
      cinematic_.letterbox_amount = 0.85f;
      cinematic_.eyebrow = "HORIZON STARTING GRID";
      cinematic_.headline = "GRID FLYBY";
      cinematic_.subheadline = "COLORADO FESTIVAL CIRCUIT";
      cinematic_.badge_text = "PRE-RACE";
      cinematic_.accent_rgba = pack_rgba(0xF9, 0x00, 0x64);
      break;

    case CampaignStage::kPostRaceFinish:
      cinematic_.kind = CutsceneKind::kPostRaceFinishCam;
      cinematic_.duration_s = 12.0;
      cinematic_.letterbox_amount = 0.72f;
      cinematic_.eyebrow = "RACE RESULTS";
      cinematic_.headline = "RACE FINISHED";
      cinematic_.subheadline = "CALCULATING FESTIVAL REWARDS";
      cinematic_.badge_text = "STAGE 1 / 3";
      cinematic_.accent_rgba = pack_rgba(0xF9, 0x00, 0x64);
      tally_.stage = PostRaceStage::kRaceResults;
      tally_.stage_elapsed_s = 0.0;
      break;

    case CampaignStage::kRewardTally:
      tally_.active = true;
      tally_.elapsed_s = 0.0;
      tally_.stage = PostRaceStage::kCreditsEarned;
      tally_.stage_elapsed_s = 0.0;
      break;

    case CampaignStage::kFinaleVictory:
      finale_complete_ = true;
      pending_darius_showdown_ = false;
      cinematic_.kind = CutsceneKind::kFinaleVictoryCinematic;
      cinematic_.duration_s = 8.5;
      cinematic_.letterbox_amount = 1.0f;
      cinematic_.eyebrow = "HORIZON FESTIVAL CHAMPION  •  COLORADO";
      cinematic_.headline = "DARIUS FLYNT DETHRONED!";
      cinematic_.subheadline = "YOU ARE THE NEW FORZA HORIZON CHAMPION  •  50,000 CR + FERRARI 599XX";
      cinematic_.reward_car = "2010 Ferrari 599XX";
      cinematic_.detail_line = "Congratulations! From rookie #250 to undisputed Horizon Champion across Colorado.";
      cinematic_.badge_text = "FESTIVAL CHAMPION";
      cinematic_.colour_key = "Gold";
      cinematic_.wristband_level = 6;
      cinematic_.boss_index = 6;
      cinematic_.accent_rgba = pack_rgba(0xFF, 0xD2, 0x3F);
      dialogs_.enqueue(
          {"finale_victory_alice", CharacterId::kAliceHart, "", "", "PostFinalRace", "",
           "You did it! You beat Darius Flynt and won the Horizon Festival! Colorado is yours — enjoy the spotlight!",
           6.5, 10, false, true});
      act.save_requested = true;
      break;

    case CampaignStage::kCredits:
      cinematic_.kind = CutsceneKind::kFinaleCreditsFmv;
      cinematic_.duration_s = 36.0;
      cinematic_.letterbox_amount = 1.0f;
      cinematic_.eyebrow = "FORZA HORIZON";
      cinematic_.headline = "HORIZON FESTIVAL CREDITS";
      cinematic_.subheadline = "COLORADO  •  ENDLESS FREE ROAM AWAITS AFTER CREDITS";
      cinematic_.hold_to_skip = true;
      cinematic_.accent_rgba = pack_rgba(0xFF, 0xD2, 0x3F);
      if (!skip_movies_) {
        act.play_fmv = true;
        act.fmv_cutscene = CutsceneKind::kFinaleCreditsFmv;
      }
      act.save_requested = true;
      break;

    case CampaignStage::kTitle:
      break;
  }
  return act;
}

CampaignAction CampaignFlow::update(double dt, std::array<double, 2> player_xz, bool fmv_active, bool race_finished) {
  dt = std::max(0.0, dt);
  stage_time_ += dt;
  dialogs_.update(dt);
  transition_.update(dt);
  if (cinematic_.kind != CutsceneKind::kNone) {
    cinematic_.elapsed_s += dt;
  }
  if (tally_.active) {
    tally_.update(dt);
  }

  CampaignAction act;
  switch (stage_) {
    case CampaignStage::kOpeningCutscene:
      if (!fmv_active && stage_time_ > 0.25) {
        mark_cutscene_done(CutsceneKind::kOpeningFmv);
        return enter_stage(CampaignStage::kViperIntroDrive, TransitionKind::kBrushWipe);
      }
      break;

    case CampaignStage::kViperIntroDrive: {
      dist_to_festival_ = std::hypot(festival_gate_[0] - player_xz[0], festival_gate_[2] - player_xz[1]);
      if (viper_beat_idx_ == 0 && stage_time_ >= 0.4) {
        viper_beat_idx_ = 1;
        dialogs_.enqueue(
            {"m0_viper_beat1", CharacterId::kScottTyler, "", "", "AllComersEventActivated", "AliceOpeningInterview1",
             "Welcome to Horizon Bass Arena! Alice Hart says only 250 festival passes remain — push that SRT Viper GTS to the main gate!",
             7.0, 6, true, false},
            &heard_dialogs_);
      } else if (viper_beat_idx_ == 1 && (dist_to_festival_ <= 2200.0 || stage_time_ >= 13.0)) {
        viper_beat_idx_ = 2;
        dialogs_.enqueue(
            {"m0_viper_beat2", CharacterId::kAliceHart, "", "", "Welcome", "FestivalTicketsDepleting9",
             "Wristbands are going fast at the Horizon Festival entrance! Follow the GPS route down the Colorado highway.",
             6.5, 6, true, false},
            &heard_dialogs_);
      } else if (viper_beat_idx_ == 2 && (dist_to_festival_ <= 900.0 || stage_time_ >= 26.0)) {
        viper_beat_idx_ = 3;
        dialogs_.enqueue(
            {"m0_viper_beat3", CharacterId::kScottTyler, "", "", "", "AliceOpeningInterview3",
             "You can hear the main stage pulsing from here! Pull into the Horizon Festival entrance to claim your qualifying spot!",
             6.5, 6, true, false},
            &heard_dialogs_);
      }
      if (dist_to_festival_ <= kFestivalArrivalRadiusM) {
        return enter_stage(CampaignStage::kQualifyRace, TransitionKind::kBrushWipe);
      }
      break;
    }

    case CampaignStage::kQualifyRace:
      if (qualify_retry_pending_) break;
      if (race_finished && qualify_launched_) {
        return enter_stage(CampaignStage::kFestivalRegistration, TransitionKind::kBrushWipe);
      }
      break;

    case CampaignStage::kFestivalRegistration:
      if (fmv_active) break;
      if (stage_time_ >= cinematic_.duration_s && !dialogs_.active()) {
        mark_cutscene_done(CutsceneKind::kWristbandCeremonyFmv);
        mark_cutscene_done(CutsceneKind::kWristbandStageCinematic);
        starter_autoshow_onboarding_ = true;
        auto out = enter_stage(CampaignStage::kFreeRoam, TransitionKind::kBrushWipe);
        out.intro_completed = true;
        out.grant_starter = true;
        out.collect_yellow_wristband = true;
        out.open_autoshow = true;
        out.set_gps = true;
        out.gps_label = "RACE CENTRAL  •  YELLOW WRISTBAND EVENTS";
        out.gps_xz = {festival_gate_[0], festival_gate_[2]};
        dialogs_.enqueue(
            {"m0_autoshow_welcome", CharacterId::kAliceHart, "", "", "", "",
             "Welcome to the Horizon Autoshow! Your 1995 VW Corrado VR6 is already in your garage — browse affordable starter rides with your 15,000 Cr bonus or exit anytime to start your Yellow Wristband events!",
             7.5, 7, true, false},
            &heard_dialogs_);
        return out;
      }
      break;

    case CampaignStage::kWristbandCeremony:
    case CampaignStage::kShowcaseIntro:
    case CampaignStage::kShowcaseOutro:
    case CampaignStage::kBossIntro:
    case CampaignStage::kBossVictory:
    case CampaignStage::kBarnDiscovery:
    case CampaignStage::kBarnRestored:
    case CampaignStage::kPreRaceGrid:
      if (stage_time_ >= cinematic_.duration_s) {
        mark_cutscene_done(cinematic_.kind);
        if (stage_ == CampaignStage::kBossVictory && pending_finale_credits_) {
          pending_finale_credits_ = false;
          return enter_stage(CampaignStage::kFinaleVictory, TransitionKind::kBrushWipe);
        }
        return enter_stage(CampaignStage::kFreeRoam, TransitionKind::kCrossFade);
      }
      break;

    case CampaignStage::kPostRaceFinish:
      if (stage_time_ >= cinematic_.duration_s) {
        mark_cutscene_done(CutsceneKind::kPostRaceFinishCam);
        return enter_stage(CampaignStage::kRewardTally, TransitionKind::kSlideLeft);
      }
      break;

    case CampaignStage::kFinaleIntro:
      if (stage_time_ >= cinematic_.duration_s) {
        mark_cutscene_done(CutsceneKind::kBossIntroCinematic);
        return enter_stage(CampaignStage::kFreeRoam, TransitionKind::kBrushWipe);
      }
      break;

    case CampaignStage::kFinaleVictory:
      if (stage_time_ >= cinematic_.duration_s) {
        mark_cutscene_done(CutsceneKind::kFinaleVictoryCinematic);
        return enter_stage(CampaignStage::kCredits, TransitionKind::kCrossFade);
      }
      break;

    case CampaignStage::kCredits:
      if (!fmv_active && stage_time_ >= cinematic_.duration_s) {
        mark_cutscene_done(CutsceneKind::kFinaleCreditsFmv);
        return enter_stage(CampaignStage::kFreeRoam, TransitionKind::kCrossFade);
      }
      break;

    case CampaignStage::kTitle:
    case CampaignStage::kFreeRoam:
    case CampaignStage::kRewardTally:
      break;
  }
  return act;
}

CampaignAction CampaignFlow::advance_or_skip() {
  switch (stage_) {
    case CampaignStage::kOpeningCutscene:
      mark_cutscene_done(CutsceneKind::kOpeningFmv);
      return enter_stage(CampaignStage::kViperIntroDrive, TransitionKind::kBrushWipe);
    case CampaignStage::kViperIntroDrive:
      return enter_stage(CampaignStage::kQualifyRace, TransitionKind::kBrushWipe);
    case CampaignStage::kQualifyRace:
      if (qualify_retry_pending_) {
        qualify_retry_pending_ = false;
        return enter_stage(CampaignStage::kQualifyRace, TransitionKind::kBrushWipe);
      }
      return enter_stage(CampaignStage::kFestivalRegistration, TransitionKind::kBrushWipe);
    case CampaignStage::kFestivalRegistration: {
      mark_cutscene_done(CutsceneKind::kWristbandCeremonyFmv);
      mark_cutscene_done(CutsceneKind::kWristbandStageCinematic);
      starter_autoshow_onboarding_ = true;
      auto out = enter_stage(CampaignStage::kFreeRoam, TransitionKind::kBrushWipe);
      out.intro_completed = true;
      out.grant_starter = true;
      out.collect_yellow_wristband = true;
      out.open_autoshow = true;
      out.set_gps = true;
      out.gps_label = "RACE CENTRAL  •  YELLOW WRISTBAND EVENTS";
      out.gps_xz = {festival_gate_[0], festival_gate_[2]};
      dialogs_.enqueue(
          {"m0_autoshow_welcome", CharacterId::kAliceHart, "", "", "", "",
           "Welcome to the Horizon Autoshow! Your 1995 VW Corrado VR6 is already in your garage — browse affordable starter rides with your 15,000 Cr bonus or exit anytime to start your Yellow Wristband events!",
           7.5, 7, true, false},
          &heard_dialogs_);
      return out;
    }
    case CampaignStage::kPostRaceFinish:
      mark_cutscene_done(CutsceneKind::kPostRaceFinishCam);
      return enter_stage(CampaignStage::kRewardTally, TransitionKind::kSlideLeft);
    case CampaignStage::kRewardTally:
      if (!tally_.stage_animation_done()) {
        tally_.skip_to_end();
        return {};
      }
      if (tally_.advance_stage()) {
        transition_.start(TransitionKind::kSlideLeft, 0.35, "");
        return {};
      }
      if (tally_.is_finale && tally_.place == 1) {
        return enter_stage(CampaignStage::kFinaleVictory, TransitionKind::kBrushWipe);
      }
      if (tally_.is_boss && tally_.place == 1 && active_boss_idx_ >= 0) {
        return enter_stage(CampaignStage::kBossVictory, TransitionKind::kBrushWipe);
      }
      if (tally_.is_showcase && tally_.place == 1 && active_showcase_idx_ >= 0) {
        return enter_stage(CampaignStage::kShowcaseOutro, TransitionKind::kBrushWipe);
      }
      return enter_stage(CampaignStage::kFreeRoam, TransitionKind::kCrossFade);
    case CampaignStage::kFinaleVictory:
      mark_cutscene_done(CutsceneKind::kFinaleVictoryCinematic);
      return enter_stage(CampaignStage::kCredits, TransitionKind::kCrossFade);
    case CampaignStage::kCredits:
      mark_cutscene_done(CutsceneKind::kFinaleCreditsFmv);
      return enter_stage(CampaignStage::kFreeRoam, TransitionKind::kCrossFade);
    case CampaignStage::kWristbandCeremony:
    case CampaignStage::kShowcaseIntro:
    case CampaignStage::kShowcaseOutro:
    case CampaignStage::kBossIntro:
    case CampaignStage::kBossVictory:
    case CampaignStage::kBarnDiscovery:
    case CampaignStage::kBarnRestored:
    case CampaignStage::kPreRaceGrid:
    case CampaignStage::kFinaleIntro:
      mark_cutscene_done(cinematic_.kind);
      return enter_stage(CampaignStage::kFreeRoam, TransitionKind::kCrossFade);
    case CampaignStage::kTitle:
    case CampaignStage::kFreeRoam:
      if (dialogs_.active()) dialogs_.skip_current();
      return {};
  }
  return {};
}

CampaignAction CampaignFlow::skip_onboarding_all() {
  mark_cutscene_done(CutsceneKind::kOpeningFmv);
  mark_cutscene_done(CutsceneKind::kWristbandCeremonyFmv);
  auto out = enter_stage(CampaignStage::kFreeRoam, TransitionKind::kBrushWipe);
  out.swap_car = true;
  out.car_media = kCorradoMedia;
  out.car_id = kCorradoCarId;
  out.teleport = true;
  out.pos = festival_gate_;
  out.heading = 3.14159265358979323846;
  out.set_gps = true;
  out.gps_label = "RACE CENTRAL  •  YELLOW WRISTBAND EVENTS";
  out.gps_xz = {festival_gate_[0], festival_gate_[2]};
  out.grant_starter = true;
  out.collect_yellow_wristband = true;
  out.intro_completed = true;
  return out;
}

CampaignAction CampaignFlow::on_enter_event(const CareerEvent& ev) {
  const std::string up_id = upper_ascii(ev.horizon_id);
  if (ev.style == 6 || up_id == "HEADLINE_EVENT" || up_id == "NEM_FINAL") {
    active_boss_idx_ = int(boss_rivals().size()) - 1;
    auto act = enter_stage(CampaignStage::kFinaleIntro, TransitionKind::kBrushWipe);
    if (up_id == "HEADLINE_EVENT" || (ev.style == 6 && up_id != "NEM_FINAL")) {
      cinematic_.eyebrow = "HORIZON FESTIVAL FINALE  •  CHAMPIONSHIP QUALIFIER";
      cinematic_.headline = "HORIZON FESTIVAL FINALE";
      cinematic_.subheadline = "DEFEAT THE TOP FINALISTS TO CHALLENGE DARIUS FLYNT";
      cinematic_.detail_line = "WIN THE FINALE TO UNLOCK THE CHAMPION VS CHAMPION SHOWDOWN";
      cinematic_.badge_text = "FESTIVAL FINALE";
    } else {
      cinematic_.headline = "DARIUS FLYNT";
    }
    return act;
  }
  if (const BossRivalSpec* boss = find_boss_by_event(ev.horizon_id)) {
    active_boss_idx_ = boss->index;
    return enter_stage(CampaignStage::kBossIntro, TransitionKind::kBrushWipe);
  }
  if (const ShowcaseMilestoneSpec* sc = find_showcase_by_event(ev.horizon_id)) {
    active_showcase_idx_ = sc->index;
    return enter_stage(CampaignStage::kShowcaseIntro, TransitionKind::kBrushWipe);
  }
  auto act = enter_stage(CampaignStage::kPreRaceGrid, TransitionKind::kSlideLeft);
  cinematic_.headline = resolve_event_display_title(&ev);
  cinematic_.subheadline = CareerCatalog::event_style_label(ev);
  return act;
}

CampaignAction CampaignFlow::on_race_finished(const CareerEvent* ev, const RaceAward& award, int place, int total_cars,
                                              int old_pop_rank, int new_pop_rank, int64_t balance_before,
                                              int64_t balance_after, int wb_points_after, int collected_wristband,
                                              const std::string& prize_car_name, const CareerCatalog* catalog,
                                              const std::vector<RaceFinishStanding>& standings) {
  auto ord = [](int p) -> const char* {
    if (p % 100 >= 11 && p % 100 <= 13) return "TH";
    return p % 10 == 1 ? "ST" : p % 10 == 2 ? "ND" : p % 10 == 3 ? "RD" : "TH";
  };

  auto fill_standings_rows = [&](const ShowcaseMilestoneSpec* matched_sc, const BossRivalSpec* matched_boss) {
    tally_.results_rows.clear();
    if (!standings.empty()) {
      std::vector<RaceFinishStanding> ordered = standings;
      std::stable_sort(ordered.begin(), ordered.end(),
                       [](const RaceFinishStanding& a, const RaceFinishStanding& b) { return a.place < b.place; });
      double winner_time = -1.0;
      for (const auto& st : ordered) {
        if (st.place == 1 && st.finish_time_s > 0.0) {
          winner_time = st.finish_time_s;
          break;
        }
      }
      for (size_t i = 0; i < ordered.size(); ++i) {
        const auto& st = ordered[i];
        RewardTallyRow row;
        row.place = st.place > 0 ? st.place : int(i) + 1;
        row.driver = !st.driver_name.empty() ? st.driver_name : (st.is_player ? "YOU" : "HORIZON DRIVER");
        row.car = st.car_name;
        row.car_class = st.class_name;
        row.pi = st.pi;
        row.time_s = st.finish_time_s;
        row.best_lap_s = st.best_lap_s;
        row.player = st.is_player;
        row.is_boss = st.is_boss;
        if (row.place == 1) {
          row.gap_s = 0.0;
        } else if (st.gap_s >= 0.0) {
          row.gap_s = st.gap_s;
        } else if (winner_time > 0.0 && st.finish_time_s > 0.0) {
          row.gap_s = std::max(0.0, st.finish_time_s - winner_time);
        } else {
          row.gap_s = 0.0;
        }
        tally_.results_rows.push_back(std::move(row));
      }
      return;
    }

    // Synthetic fallback (used only in headless/unit tests when RaceView standings are absent).
    if (matched_sc) {
      RewardTallyRow player_row;
      player_row.place = tally_.place;
      player_row.driver = "YOU";
      player_row.car = matched_sc->loaner_car_display;
      player_row.gap_s = tally_.place == 1 ? 0.0 : 1.0;
      player_row.player = true;
      RewardTallyRow opp_row;
      opp_row.place = tally_.place == 1 ? 2 : 1;
      opp_row.driver = matched_sc->opponent_label;
      opp_row.car = matched_sc->airborne ? "AIRBORNE CHALLENGER" : "SHOWCASE FIELD";
      opp_row.gap_s = tally_.place == 1 ? 1.0 : 0.0;
      if (tally_.place == 1) {
        tally_.results_rows.push_back(std::move(player_row));
        tally_.results_rows.push_back(std::move(opp_row));
      } else {
        tally_.results_rows.push_back(std::move(opp_row));
        tally_.results_rows.push_back(std::move(player_row));
      }
    } else if (matched_boss) {
      RewardTallyRow player_row;
      player_row.place = tally_.place;
      player_row.driver = "YOU";
      player_row.car = "FESTIVAL CHALLENGER";
      player_row.gap_s = tally_.place == 1 ? 0.0 : 0.85;
      player_row.player = true;
      RewardTallyRow rival_row;
      rival_row.place = tally_.place == 1 ? 2 : 1;
      rival_row.driver = matched_boss->rival_name;
      rival_row.car = matched_boss->car_display;
      rival_row.gap_s = tally_.place == 1 ? 0.85 : 0.0;
      rival_row.is_boss = true;
      if (tally_.place == 1) {
        tally_.results_rows.push_back(std::move(player_row));
        tally_.results_rows.push_back(std::move(rival_row));
      } else {
        tally_.results_rows.push_back(std::move(rival_row));
        tally_.results_rows.push_back(std::move(player_row));
      }
    } else {
      const int n = std::clamp(tally_.total_cars, 1, 8);
      int rival_num = 1;
      for (int p = 1; p <= n; ++p) {
        const bool is_me = (p == tally_.place);
        RewardTallyRow row;
        row.place = p;
        row.player = is_me;
        row.driver = is_me ? "YOU" : ("HORIZON DRIVER " + std::to_string(rival_num++));
        row.car = is_me ? "YOUR VEHICLE" : tally_.event_category;
        row.gap_s = p == 1 ? 0.0 : double(p - 1);
        tally_.results_rows.push_back(std::move(row));
      }
    }
  };

  // R3-CAM-08: Corrado Qualification Race (FESTIVAL_SECOND) enforces a top-3 podium cutoff.
  // Finishing 1st..3rd advances to Festival Registration; finishing worse than 3rd requires retry
  // and MUST NOT advance CampaignStage past kQualifyRace.
  if (stage_ == CampaignStage::kQualifyRace) {
    const int eff_place = std::max(1, place);
    if (eff_place <= kQualifyCutoffPlace) {
      qualify_retry_pending_ = false;
      tally_.qualified = true;
      tally_.retry_required = false;
      return enter_stage(CampaignStage::kFestivalRegistration, TransitionKind::kBrushWipe);
    }
    qualify_retry_pending_ = true;
    qualify_launched_ = false;
    stage_time_ = 0.0;
    tally_ = RewardTallyState{};
    tally_.active = true;
    tally_.stage = PostRaceStage::kRaceResults;
    tally_.place = eff_place;
    tally_.total_cars = std::max(eff_place, total_cars);
    tally_.event_title = "HORIZON QUALIFIER";
    tally_.event_category = "QUALIFICATION RACE";
    tally_.qualified = false;
    tally_.retry_required = true;
    tally_.qualify_cutoff_place = kQualifyCutoffPlace;
    tally_.next_unlock_headline = "DID NOT QUALIFY — TOP 3 REQUIRED";
    tally_.next_unlock_detail = "PRESS CONTINUE TO RETRY THE HORIZON QUALIFICATION RACE";
    fill_standings_rows(nullptr, nullptr);

    cinematic_ = CinematicBanner{};
    cinematic_.kind = CutsceneKind::kPostRaceFinishCam;
    cinematic_.duration_s = 12.0;
    cinematic_.letterbox_amount = 0.72f;
    cinematic_.eyebrow = "QUALIFICATION FAILED  •  TOP 3 REQUIRED";
    cinematic_.headline = std::to_string(eff_place) + ord(eff_place) + " PLACE  —  DID NOT QUALIFY";
    cinematic_.subheadline = "FINISH IN THE TOP 3 TO EARN YOUR YELLOW WRISTBAND  •  PRESS CONTINUE TO RETRY";
    cinematic_.detail_line = "Line back up in the 1995 VW Corrado VR6 and finish 3rd or better to qualify.";
    cinematic_.badge_text = "RETRY QUALIFICATION";
    cinematic_.accent_rgba = pack_rgba(0xF9, 0x00, 0x64);
    dialogs_.enqueue(
        {"m0_qualify_retry", CharacterId::kAliceHart, "", "", "FirstRaceLost", "",
         "Tough break — you need a top-3 finish to earn your Yellow Wristband! Line back up on the grid and give it another shot.",
         5.5, 8, false, true});
    return {};
  }

  on_popularity_rank_changed(old_pop_rank, new_pop_rank);
  on_wristband_points_changed(wb_points_after, collected_wristband);

  tally_ = RewardTallyState{};
  tally_.active = true;
  tally_.stage = PostRaceStage::kRaceResults;
  tally_.place = std::max(1, place);
  tally_.total_cars = std::max(1, total_cars);
  tally_.event_title = resolve_event_display_title(ev);
  tally_.event_category = ev ? CareerCatalog::event_style_label(*ev) : "FESTIVAL RACE";
  tally_.base_credits = award.base_credits;
  tally_.mileage_bonus = award.mileage_bonus;
  tally_.difficulty_pct = award.difficulty_pct;
  tally_.difficulty_bonus = award.difficulty_bonus;
  tally_.assists_pct = award.assists_pct;
  tally_.assists_bonus = award.assists_bonus;
  tally_.skills_bonus = award.skills_bonus;
  tally_.nemesis_bonus = award.nemesis_bonus;
  tally_.total_credits = award.credits;
  tally_.balance_before = balance_before;
  tally_.balance_after = balance_after;
  tally_.pop_rank_before = std::clamp(old_pop_rank, 1, 250);
  tally_.pop_rank_after = std::clamp(new_pop_rank, 1, 250);
  tally_.wb_points_earned = award.wristband_points;
  tally_.wb_after = wb_points_after;
  tally_.wb_before = std::max(0, wb_points_after - award.wristband_points);
  tally_.prize_car_name = prize_car_name;

  const auto& bands = wristband_milestones();
  const int cur_i = std::clamp(collected_wristband, 0, int(bands.size()) - 1);
  tally_.wb_level = cur_i;
  tally_.wb_current_name = bands[size_t(cur_i)].display_name;
  tally_.wb_tier_lo = bands[size_t(cur_i)].xp_threshold;
  if (cur_i + 1 < int(bands.size())) {
    tally_.wb_tier_hi = bands[size_t(cur_i + 1)].xp_threshold;
    tally_.wb_next_name = bands[size_t(cur_i + 1)].display_name;
    tally_.wb_tier_up = (wb_points_after >= tally_.wb_tier_hi);
  } else {
    tally_.wb_tier_hi = tally_.wb_tier_lo;
    tally_.wb_next_name = "HORIZON FINALE";
    tally_.wb_tier_up = false;
  }

  bool triggered_darius_unlock = false;
  const ShowcaseMilestoneSpec* matched_sc = nullptr;
  const BossRivalSpec* matched_boss = nullptr;
  if (ev) {
    if (const BossRivalSpec* b = find_boss_by_event(ev->horizon_id)) {
      matched_boss = b;
      tally_.is_boss = true;
      active_boss_idx_ = b->index;
      if (b->is_finale_champion && place == 1) {
        tally_.is_finale = true;
        finale_complete_ = true;
        pending_darius_showdown_ = false;
      }
    } else if (ev->style == 6 || upper_ascii(ev->horizon_id) == "HEADLINE_EVENT") {
      // Winning the Horizon Festival Finale unlocks Darius Flynt's 1-on-1 Champion vs Champion
      // Showdown rather than skipping straight to Credits!
      if (place == 1) {
        pending_darius_showdown_ = true;
        triggered_darius_unlock = true;
        unlocked_bosses_.insert("darius");
        completed_cutscenes_.insert("headline_event_won");
      }
    } else if (const ShowcaseMilestoneSpec* sc = find_showcase_by_event(ev->horizon_id)) {
      matched_sc = sc;
      tally_.is_showcase = true;
      active_showcase_idx_ = sc->index;
    }
  }

  // Populate Stage 1 (RACE RESULTS) standings from actual RaceView standings when provided.
  fill_standings_rows(matched_sc, matched_boss);

  if (triggered_darius_unlock) {
    tally_.next_unlock_headline = "DARIUS FLYNT SHOWDOWN UNLOCKED!";
    tally_.next_unlock_detail = "FACE DARIUS FLYNT IN THE CHAMPION VS CHAMPION SHOWDOWN";
  } else if (tally_.wb_tier_up) {
    tally_.next_unlock_headline = tally_.wb_next_name + " READY!";
    tally_.next_unlock_detail = "RETURN TO RACE CENTRAL TO COLLECT YOUR WRISTBAND";
  } else if (tally_.is_finale && place == 1) {
    tally_.next_unlock_headline = "HORIZON FESTIVAL CHAMPION!";
    tally_.next_unlock_detail = "50,000 CR + 2010 FERRARI 599XX  •  PROCEED TO CHAMPIONSHIP CELEBRATION";
  } else if (!prize_car_name.empty()) {
    tally_.next_unlock_headline = "NEW PRIZE CAR WON!";
    tally_.next_unlock_detail = prize_car_name + " DELIVERED TO YOUR GARAGE";
  }

  const CampaignObjective obj = next_objective(wb_points_after, collected_wristband, new_pop_rank, catalog);
  if (obj.valid) {
    tally_.has_next_gps = true;
    tally_.next_gps_label = obj.gps_label;
    tally_.next_gps_xz = obj.gps_xz;
  }

  // Trigger post-race voice line.
  if (triggered_darius_unlock) {
    dialogs_.enqueue(
        {"darius_challenge_headline", CharacterId::kDariusFlynt, "", "", "PostFinalRace", "",
         "Not bad, rookie. You took the Horizon Finale — now meet me one-on-one in the Champion vs Champion Showdown for the crown and my Ferrari 599XX.",
         6.5, 10, false, true});
  } else if (place == 1) {
    dialogs_.enqueue(
        {"post_race_win_" + tally_.event_title, CharacterId::kAliceHart, "", "", "FirstPostRaceWinner", "",
         "Brilliant win! The crowd at the Festival is loving it — check your rewards and next unlocked events.",
         5.0, 5, false, false});
  } else if (place <= 3) {
    dialogs_.enqueue(
        {"post_race_podium_" + tally_.event_title, CharacterId::kHollyCruz, "", "", "FirstPostRacePodium", "",
         "Solid podium finish! Keep banking points to climb to the next Horizon Wristband tier.",
         5.0, 4, false, false});
  }

  auto act = enter_stage(CampaignStage::kPostRaceFinish, TransitionKind::kCrossFade);
  cinematic_.eyebrow = "RACE RESULTS  •  " + tally_.event_category;
  cinematic_.headline = std::to_string(tally_.place) + ord(tally_.place) + " PLACE";
  cinematic_.subheadline = tally_.event_title;
  cinematic_.badge_text = "STAGE 1 / 3  •  FINISHED";
  act.save_requested = true;
  return act;
}

bool CampaignFlow::on_viper_convoy_pass(int passed_delta) {
  if (stage_ != CampaignStage::kViperIntroDrive || passed_delta <= 0 || viper_convoy_passed_ >= kViperConvoyTarget) {
    return false;
  }
  const int prev = viper_convoy_passed_;
  viper_convoy_passed_ = std::min(kViperConvoyTarget, viper_convoy_passed_ + passed_delta);
  const int rem = viper_convoy_remaining();
  if (prev == 0 && viper_convoy_passed_ >= 1) {
    dialogs_.enqueue(
        {"m0_viper_convoy_first", CharacterId::kScottTyler, "", "", "FestivalTicketsDepleting9",
         "FestivalTicketsDepleting9",
         "Nice pass! Keep weaving through the Festival-bound convoy — " + std::to_string(rem) +
             " drivers left to overtake!",
         5.0, 6, true, false},
        &heard_dialogs_);
  } else if (rem == 1 && prev < kViperConvoyTarget - 1) {
    dialogs_.enqueue(
        {"m0_viper_convoy_last", CharacterId::kAliceHart, "", "", "FestivalTicketsDepleting5", "",
         "Only one Festival driver left ahead of you — push that SRT Viper GTS to the main gate!", 4.8, 6, true,
         false},
        &heard_dialogs_);
  } else if (viper_convoy_passed_ >= kViperConvoyTarget && prev < kViperConvoyTarget) {
    dialogs_.enqueue(
        {"m0_viper_convoy_clear", CharacterId::kScottTyler, "", "", "AliceOpeningInterview3", "AliceOpeningInterview3",
         "You blew past the entire Festival convoy! Pull into the Horizon Festival entrance to claim your spot!", 5.5,
         7, true, false},
        &heard_dialogs_);
  }
  return true;
}

CampaignAction CampaignFlow::on_wristband_collected(int wristband_level, const std::string& bonus_car_name,
                                                    int newly_unlocked_events) {
  active_wristband_ = std::clamp(wristband_level, 0, int(wristband_milestones().size()) - 1);
  auto act = enter_stage(CampaignStage::kWristbandCeremony, TransitionKind::kBrushWipe);
  if (!bonus_car_name.empty()) cinematic_.reward_car = bonus_car_name;
  if (newly_unlocked_events > 0) {
    cinematic_.subheadline = std::to_string(newly_unlocked_events) + " NEW FESTIVAL EVENTS UNLOCKED";
  }
  if (const WristbandMilestoneSpec* wb = find_wristband_milestone(active_wristband_)) {
    dialogs_.enqueue(
        {"wb_collect_" + std::string(wb->key), CharacterId::kAliceHart, "", "", wb->vo_unlock_trigger, wb->radio_event,
         wb->announcement, 6.0, 8, true, true},
        &heard_dialogs_);
  }
  act.save_requested = true;
  return act;
}

CampaignAction CampaignFlow::on_barn_find(bool discovered, const std::string& barn_id, const std::string& car_name) {
  if (!discovered) {
    dialogs_.enqueue(
        {"barn_rumour_" + barn_id, CharacterId::kDakStewart, "", "", "BarnFindFirstRumour", "",
         "Hey, word around the Festival is someone stashed a classic in a barn nearby. Check your map for the purple search zone!",
         6.0, 6, true, false},
        &heard_dialogs_);
    return {};
  }
  auto act = enter_stage(CampaignStage::kBarnDiscovery, TransitionKind::kCrossFade);
  cinematic_.headline = car_name.empty() ? "BARN FIND DISCOVERED!" : car_name;
  cinematic_.detail_line = "Dak's recovery crew is hauling this Colorado classic back to the Horizon Garage.";
  cinematic_.reward_car = car_name;
  dialogs_.enqueue(
      {"barn_found_" + barn_id, CharacterId::kDakStewart, "", "", "BarnFindIntro", "",
       "Look at what you found! Let me get it back to the shop, strip the rust, and tune it back to life.",
       6.0, 8, true, true},
      &heard_dialogs_);
  act.save_requested = true;
  return act;
}

CampaignAction CampaignFlow::on_barn_restored(const std::string& barn_id, const std::string& car_name) {
  auto act = enter_stage(CampaignStage::kBarnRestored, TransitionKind::kBrushWipe);
  cinematic_.headline = car_name.empty() ? "RESTORATION COMPLETE" : car_name;
  cinematic_.reward_car = car_name;
  cinematic_.detail_line = "Fresh paint, rebuilt engine, and ready for the Colorado open road.";
  dialogs_.enqueue(
      {"barn_restored_" + barn_id, CharacterId::kDakStewart, "", "", "1040BarnFindAtWorkshop", "",
       "Good news — your barn find restoration is finished and gleaming in the Horizon Garage. Come take her for a spin!",
       6.0, 7, true, true},
      &heard_dialogs_);
  act.save_requested = true;
  return act;
}

void CampaignFlow::on_popularity_rank_changed(int old_rank, int new_rank) {
  if (new_rank >= old_rank) return;
  check_showcase_unlocks(new_rank);
}

void CampaignFlow::on_wristband_points_changed(int points, int collected_wristband) {
  check_boss_unlocks(points, collected_wristband);
}

void CampaignFlow::check_boss_unlocks(int wristband_points, int collected_wristband) {
  for (const auto& boss : boss_rivals()) {
    if (boss.is_finale_champion) continue;  // Darius (NEM_FINAL) unlocks only after winning HEADLINE_EVENT.
    if (unlocked_bosses_.count(boss.key) > 0) continue;
    if (collected_wristband >= boss.wristband_level && wristband_points >= boss.unlock_points) {
      unlocked_bosses_.insert(boss.key);
      dialogs_.enqueue(
          {"boss_unlock_" + std::string(boss.key), boss.character, "", "", boss.vo_unlocked, "",
           std::string("Star Showdown unlocked! ") + boss.rival_name + " challenges you in the " + boss.car_display + ".",
           6.0, 7, true, false},
          &heard_dialogs_);
    }
  }
}

void CampaignFlow::check_showcase_unlocks(int pop_rank) {
  for (const auto& sc : showcase_milestones()) {
    if (unlocked_showcases_.count(sc.horizon_id) > 0) continue;
    if (pop_rank <= sc.popularity_rank_req) {
      unlocked_showcases_.insert(sc.horizon_id);
      dialogs_.enqueue(
          {"sc_unlock_" + std::string(sc.horizon_id), CharacterId::kHollyCruz, "", "", "ShowcaseUnlockedFreeRoam", "",
           std::string("New Showcase Event unlocked: ") + sc.display_name + " (" + sc.loaner_car_display + " vs " +
               sc.opponent_label + ")!",
           6.0, 6, true, false},
          &heard_dialogs_);
    }
  }
}

void CampaignFlow::start_credits() { enter_stage(CampaignStage::kCredits, TransitionKind::kCrossFade); }

bool CampaignFlow::jump_to_cli_stage(std::string_view cli_stage, CampaignAction& out_action) {
  const std::string k = lower_ascii(cli_stage);
  out_action = CampaignAction{};
  dialogs_.clear();
  heard_dialogs_.clear();

  if (k == "intro" || k == "opening") {
    skip_movies_ = false;
    out_action = enter_stage(CampaignStage::kOpeningCutscene, TransitionKind::kCrossFade);
    return true;
  }
  if (k == "viper_drive" || k == "viper" || k == "m0_intro") {
    out_action = enter_stage(CampaignStage::kViperIntroDrive, TransitionKind::kNone);
    stage_time_ = 2.4;  // HUD fully revealed + first Scott Tyler / Alice Hart call-in active
    dist_to_festival_ = std::hypot(festival_gate_[0] - viper_spawn_[0], festival_gate_[2] - viper_spawn_[2]);
    viper_beat_idx_ = 1;
    dialogs_.enqueue(
        {"cli_viper", CharacterId::kScottTyler, "", "", "AllComersEventActivated", "AliceOpeningInterview1",
         "Welcome to Horizon Bass Arena! Alice Hart says only 250 festival passes remain — push that SRT Viper GTS to the Horizon Festival!",
         9.0, 6, false, true});
    dialogs_.update(1.1);
    return true;
  }
  if (k == "dialog" || k == "dialog_subtitles" || k == "freeroam_dialog") {
    out_action = enter_stage(CampaignStage::kFreeRoam, TransitionKind::kNone);
    out_action.set_gps = true;
    out_action.gps_label = "RACE CENTRAL  •  YELLOW WRISTBAND";
    out_action.gps_xz = {festival_gate_[0], festival_gate_[2]};
    dialogs_.enqueue(
        {"cli_alice_dialog", CharacterId::kAliceHart, "", "", "FirstWristbandCollect", "",
         "You've got the whole Horizon Festival watching now — follow the GPS to Race Central to collect your next Wristband and unlock the Star Showdown!",
         10.0, 8, false, true});
    dialogs_.update(1.3);
    return true;
  }
  if (k == "registration") {
    skip_movies_ = true;
    out_action = enter_stage(CampaignStage::kFestivalRegistration, TransitionKind::kNone);
    stage_time_ = 1.6;
    dialogs_.update(1.2);
    return true;
  }
  if (k.rfind("wristband_", 0) == 0 || k == "wristband") {
    int lvl = 0;
    if (k == "wristband_green") lvl = 1;
    else if (k == "wristband_blue") lvl = 2;
    else if (k == "wristband_pink") lvl = 3;
    else if (k == "wristband_orange") lvl = 4;
    else if (k == "wristband_purple") lvl = 5;
    else if (k == "wristband_gold") lvl = 6;
    out_action = on_wristband_collected(lvl, lvl == 0 ? "1995 Volkswagen Corrado VR6" : "2012 Ford Mustang Boss 302", 0);
    stage_time_ = 1.8;
    dialogs_.update(1.2);
    return true;
  }
  if (k == "reward_tally" || k == "tally" || k == "reward_tally_credits" || k == "reward_tally_wristband" ||
      k == "reward_tally_results") {
    RaceAward aw;
    aw.base_credits = 18000;
    aw.mileage_bonus = 2400;
    aw.difficulty_pct = 30;
    aw.difficulty_bonus = 5400;
    aw.assists_pct = 20;
    aw.assists_bonus = 3600;
    aw.skills_bonus = 4250;
    aw.nemesis_bonus = 2500;
    aw.credits = 36150;
    aw.wristband_points = 180;
    CareerEvent ev;
    ev.horizon_id = "RED ROCK CANYON RUN";
    ev.style = 1;
    out_action = on_race_finished(&ev, aw, 1, 8, 224, 198, 84500, 120650, 790, 1, "2012 Nissan GT-R Black Edition");
    if (k == "reward_tally_results") {
      stage_ = CampaignStage::kPostRaceFinish;
      tally_.stage = PostRaceStage::kRaceResults;
      tally_.stage_elapsed_s = 1.0;
    } else if (k == "reward_tally_wristband") {
      stage_ = CampaignStage::kRewardTally;
      tally_.active = true;
      tally_.stage = PostRaceStage::kWristbandAndPrize;
      tally_.stage_elapsed_s = 1.6;
      tally_.elapsed_s = 3.2;
    } else {
      stage_ = CampaignStage::kRewardTally;
      tally_.active = true;
      tally_.stage = PostRaceStage::kCreditsEarned;
      tally_.stage_elapsed_s = 1.8;
      tally_.elapsed_s = 3.2;
    }
    return true;
  }
  if (k == "showcase" || k == "showcase_intro") {
    active_showcase_idx_ = 0;  // Mustang vs P-51 Mustang
    out_action = enter_stage(CampaignStage::kShowcaseIntro, TransitionKind::kNone);
    out_action.swap_car = true;
    out_action.car_media = "FOR_MustangBOSS429_70";
    out_action.car_id = 404;
    stage_time_ = 1.6;
    dialogs_.update(1.2);
    return true;
  }
  if (k.rfind("boss_", 0) == 0) {
    const std::string bkey = k.substr(5);
    const BossRivalSpec* boss = find_boss_by_key(bkey);
    active_boss_idx_ = boss ? boss->index : int(boss_rivals().size()) - 1;
    const auto& spec = boss_rivals()[size_t(active_boss_idx_)];
    out_action = enter_stage(spec.is_finale_champion ? CampaignStage::kFinaleIntro : CampaignStage::kBossIntro,
                             TransitionKind::kNone);
    out_action.swap_car = true;
    out_action.car_media = spec.car_media;
    out_action.car_id = spec.car_id;
    stage_time_ = 1.8;
    dialogs_.update(1.3);
    return true;
  }
  if (k == "barn_find" || k == "barn_discovery") {
    out_action = on_barn_find(true, "barnfind_01", "1956 Ford F-100");
    stage_time_ = 1.6;
    dialogs_.update(1.2);
    return true;
  }
  if (k == "barn_restored") {
    out_action = on_barn_restored("barnfind_01", "1964 Aston Martin DB5 Vantage");
    stage_time_ = 1.6;
    dialogs_.update(1.2);
    return true;
  }
  if (k == "finale_intro" || k == "darius_showdown") {
    active_boss_idx_ = int(boss_rivals().size()) - 1;
    const auto& spec = boss_rivals()[size_t(active_boss_idx_)];
    out_action = enter_stage(CampaignStage::kFinaleIntro, TransitionKind::kNone);
    out_action.swap_car = true;
    out_action.car_media = spec.car_media;
    out_action.car_id = spec.car_id;
    stage_time_ = 1.8;
    dialogs_.update(1.3);
    return true;
  }
  if (k == "finale" || k == "finale_victory") {
    out_action = enter_stage(CampaignStage::kFinaleVictory, TransitionKind::kNone);
    out_action.swap_car = true;
    out_action.car_media = "FER_599XX_10";
    out_action.car_id = 1171;
    stage_time_ = 2.0;
    dialogs_.update(1.3);
    return true;
  }
  if (k == "credits") {
    skip_movies_ = true;
    out_action = enter_stage(CampaignStage::kCredits, TransitionKind::kNone);
    stage_time_ = 4.5;
    return true;
  }

  CampaignStage parsed = CampaignStage::kFreeRoam;
  if (parse_campaign_stage(k, parsed)) {
    out_action = enter_stage(parsed, TransitionKind::kNone);
    stage_time_ = 1.5;
    return true;
  }
  return false;
}

void CampaignFlow::save_to(CampaignSaveState& out) const {
  out.campaign_stage = campaign_stage_name(stage_);
  out.completed_cutscenes.assign(completed_cutscenes_.begin(), completed_cutscenes_.end());
  out.heard_dialogs.assign(heard_dialogs_.begin(), heard_dialogs_.end());
  out.unlocked_bosses.assign(unlocked_bosses_.begin(), unlocked_bosses_.end());
  out.unlocked_showcases.assign(unlocked_showcases_.begin(), unlocked_showcases_.end());
  out.finale_complete = finale_complete_;
}

void CampaignFlow::load_from(const CampaignSaveState& in) {
  CampaignStage parsed = CampaignStage::kFreeRoam;
  if (!in.campaign_stage.empty() && parse_campaign_stage(in.campaign_stage, parsed)) {
    // Never boot straight into transient cinematics on reload; resume at safe hub stage.
    if (parsed == CampaignStage::kOpeningCutscene || parsed == CampaignStage::kViperIntroDrive ||
        parsed == CampaignStage::kQualifyRace || parsed == CampaignStage::kFestivalRegistration ||
        parsed == CampaignStage::kFreeRoam || parsed == CampaignStage::kTitle) {
      stage_ = parsed;
    } else {
      stage_ = CampaignStage::kFreeRoam;
    }
  } else {
    stage_ = CampaignStage::kFreeRoam;
  }
  completed_cutscenes_.clear();
  completed_cutscenes_.insert(in.completed_cutscenes.begin(), in.completed_cutscenes.end());
  heard_dialogs_.clear();
  heard_dialogs_.insert(in.heard_dialogs.begin(), in.heard_dialogs.end());
  unlocked_bosses_.clear();
  for (const auto& b : in.unlocked_bosses) {
    unlocked_bosses_.insert(lower_ascii(b) == "haley" ? std::string("hailey") : b);
  }
  unlocked_showcases_.clear();
  unlocked_showcases_.insert(in.unlocked_showcases.begin(), in.unlocked_showcases.end());
  finale_complete_ = in.finale_complete;
  pending_darius_showdown_ = !finale_complete_ && completed_cutscenes_.count("headline_event_won") > 0;
}

CampaignSaveState infer_campaign_state(const CampaignImportInputs& in) {
  std::map<int64_t, int> best;
  for (const auto& [id, place] : in.event_results) {
    auto it = best.find(id);
    if (it == best.end() || (place > 0 && place < it->second)) best[id] = place;
  }
  const std::set<int64_t> unlocks(in.event_unlocks.begin(), in.event_unlocks.end());
  auto raced = [&](int64_t id) { return best.count(id) > 0; };
  auto won = [&](int64_t id) {
    auto it = best.find(id);
    return it != best.end() && it->second == 1;
  };
  std::set<std::string> bosses, showcases, heard, cutscenes;
  bool finale = false;
  for (const auto& boss : boss_rivals()) {
    if (boss.is_finale_champion) {
      // NEM_FINAL unlocks only from a HEADLINE_EVENT win (Rewards_EventUnlock).
      if (won(boss.event_id)) finale = true;
      continue;
    }
    const bool rule = in.collected_wristband >= boss.wristband_level && in.wristband_points >= boss.unlock_points;
    if (rule || raced(boss.event_id)) {
      bosses.insert(boss.key);
      heard.insert("boss_unlock_" + std::string(boss.key));
    }
  }
  constexpr int64_t kHeadline = 167;  // HEADLINE_EVENT (Rewards_EventUnlock 167 -> 169)
  if (won(kHeadline) || finale) {
    bosses.insert("darius");
    cutscenes.insert("headline_event_won");
  }
  if (finale) {
    cutscenes.insert(cutscene_id_name(CutsceneKind::kFinaleVictoryCinematic));
    cutscenes.insert(cutscene_id_name(CutsceneKind::kFinaleCreditsFmv));
  }
  for (const auto& sc : showcase_milestones()) {
    const bool gate = in.popularity_rank > 0 ? in.popularity_rank <= sc.popularity_rank_req : unlocks.count(sc.event_id) > 0;
    if (gate || raced(sc.event_id)) {
      showcases.insert(sc.horizon_id);
      heard.insert("sc_unlock_" + std::string(sc.horizon_id));
    }
  }
  const auto& bands = wristband_milestones();
  for (int lv = 0; lv <= std::min(in.collected_wristband, int(bands.size()) - 1); ++lv)
    heard.insert("wb_collect_" + std::string(bands[size_t(lv)].key));
  if (in.intro_completed) {
    for (const char* k : {"m0_viper_beat1", "m0_viper_beat2", "m0_viper_beat3", "m0_viper_convoy_first",
                          "m0_viper_convoy_last", "m0_viper_convoy_clear", "m0_corrado_qualify", "m0_alice_welcome",
                          "m0_dak_garage", "m0_holly_events", "m0_autoshow_welcome"})
      heard.insert(k);
    cutscenes.insert(cutscene_id_name(CutsceneKind::kOpeningFmv));
    cutscenes.insert(cutscene_id_name(CutsceneKind::kWristbandCeremonyFmv));
    cutscenes.insert(cutscene_id_name(CutsceneKind::kWristbandStageCinematic));
  }
  CampaignSaveState out;
  out.campaign_stage = in.intro_completed ? "freeroam" : "";
  out.completed_cutscenes.assign(cutscenes.begin(), cutscenes.end());
  out.heard_dialogs.assign(heard.begin(), heard.end());
  out.unlocked_bosses.assign(bosses.begin(), bosses.end());
  out.unlocked_showcases.assign(showcases.begin(), showcases.end());
  out.finale_complete = finale;
  return out;
}

void seed_imported_campaign(drive::DriveSave& save, int popularity_rank) {
  CampaignImportInputs in;
  in.event_results = save.event_results;
  in.event_unlocks = save.event_unlocks;
  in.wristband_points = save.wristband_points;
  in.collected_wristband = save.collected_wristband;
  in.popularity_rank = popularity_rank;
  in.intro_completed = save.intro_completed;
  const CampaignSaveState st = infer_campaign_state(in);
  auto unite = [](std::vector<std::string>& into, const std::vector<std::string>& add) {
    std::set<std::string> all(into.begin(), into.end());
    all.insert(add.begin(), add.end());
    into.assign(all.begin(), all.end());
  };
  unite(save.completed_cutscenes, st.completed_cutscenes);
  unite(save.heard_dialogs, st.heard_dialogs);
  unite(save.unlocked_bosses, st.unlocked_bosses);
  unite(save.unlocked_showcases, st.unlocked_showcases);
  save.finale_complete = save.finale_complete || st.finale_complete;
  if (save.campaign_stage.empty()) save.campaign_stage = st.campaign_stage;
}

bool imported_campaign_needs_seed(const drive::DriveSave& save) {
  return save.owned_garage && save.completed_cutscenes.empty() && save.heard_dialogs.empty() &&
         save.unlocked_bosses.empty() && save.unlocked_showcases.empty() && !save.finale_complete;
}

CampaignObjective CampaignFlow::next_objective(int wristband_points, int collected_wristband, int pop_rank,
                                               const CareerCatalog* catalog) const {
  CampaignObjective obj;
  obj.gps_xz = {festival_gate_[0], festival_gate_[2]};
  if (catalog) {
    if (const FestivalSite* rc = catalog->find_site(FestivalSite::kRaceCentral)) {
      obj.gps_xz = {rc->pos[0], rc->pos[2]};
    }
  }

  if (stage_ == CampaignStage::kViperIntroDrive) {
    obj.valid = true;
    obj.title = "DRIVE TO THE HORIZON FESTIVAL";
    obj.detail = "2013 SRT VIPER GTS  •  FOLLOW THE GPS TO THE HORIZON FESTIVAL";
    obj.gps_label = "HORIZON FESTIVAL";
    obj.gps_xz = {festival_gate_[0], festival_gate_[2]};
    obj.distance_m = dist_to_festival_;
    obj.is_urgent = true;
    return obj;
  }
  if (stage_ == CampaignStage::kQualifyRace) {
    obj.valid = true;
    obj.title = qualify_retry_pending_ ? "RETRY QUALIFICATION RACE" : "QUALIFICATION RACE";
    obj.detail = qualify_retry_pending_
                     ? "TOP 3 FINISH REQUIRED  •  RETRY IN 1995 VW CORRADO VR6"
                     : "1995 VOLKSWAGEN CORRADO VR6  •  FINISH TOP 3 FOR YELLOW WRISTBAND";
    obj.gps_label = "HORIZON QUALIFIER";
    obj.is_urgent = true;
    return obj;
  }

  // 1. Pending wristband ready to collect at Race Central!
  const auto& bands = wristband_milestones();
  const int next_wb = collected_wristband + 1;
  if (next_wb >= 0 && next_wb < int(bands.size()) && wristband_points >= bands[size_t(next_wb)].xp_threshold) {
    obj.valid = true;
    obj.title = std::string("COLLECT ") + bands[size_t(next_wb)].display_name;
    obj.detail = "DRIVE TO RACE CENTRAL TO UNLOCK NEW EVENTS";
    obj.gps_label = "RACE CENTRAL  •  WRISTBAND CEREMONY";
    obj.is_urgent = true;
    return obj;
  }

  // 2. Champion vs Champion Showdown (Darius Flynt) after winning HEADLINE_EVENT!
  if (!finale_complete_ && pending_darius_showdown_) {
    obj.valid = true;
    obj.title = "CHAMPION VS CHAMPION: DARIUS FLYNT";
    obj.detail = "BEAT DARIUS FLYNT FOR THE HORIZON CROWN & 2010 FERRARI 599XX";
    obj.gps_label = "CHAMPION VS CHAMPION  •  DARIUS FLYNT";
    obj.is_urgent = true;
    if (catalog) {
      if (const CareerEvent* dev = catalog->find_event("NEM_FINAL"); dev && dev->has_trigger) {
        obj.gps_xz = {dev->trigger_pos[0], dev->trigger_pos[2]};
      }
    }
    return obj;
  }

  // 2b. Horizon Finale (HEADLINE_EVENT) when Gold Wristband + 10,000 pts reached!
  if (!finale_complete_ && collected_wristband >= 6 && wristband_points >= 10000) {
    obj.valid = true;
    obj.title = "HORIZON FESTIVAL FINALE";
    obj.detail = "WIN THE FINALE TO CHALLENGE DARIUS FLYNT FOR THE HORIZON CROWN";
    obj.gps_label = "HORIZON FINALE  •  HEADLINE EVENT";
    obj.is_urgent = true;
    if (catalog) {
      if (const CareerEvent* fev = catalog->find_event("HEADLINE_EVENT"); fev && fev->has_trigger) {
        obj.gps_xz = {fev->trigger_pos[0], fev->trigger_pos[2]};
      }
    }
    return obj;
  }

  // 3. Available Star Showdown boss rival for current wristband tier.
  if (const BossRivalSpec* boss = find_boss_by_level(std::clamp(collected_wristband, 0, 6))) {
    if (wristband_points >= boss->unlock_points) {
      obj.valid = true;
      obj.title = std::string("STAR SHOWDOWN: ") + boss->rival_name;
      obj.detail = std::string("BEAT THE ") + boss->car_display + " TO WIN THE PINK SLIP";
      obj.gps_label = std::string("STAR SHOWDOWN  •  ") + boss->rival_name;
      obj.is_urgent = true;
      if (catalog) {
        if (const CareerEvent* bev = catalog->find_event(boss->horizon_id); bev && bev->has_trigger) {
          obj.gps_xz = {bev->trigger_pos[0], bev->trigger_pos[2]};
        }
      }
      return obj;
    }
  }

  // 4. Default: earn points toward next wristband or enjoy free roam post-finale.
  if (finale_complete_) {
    obj.valid = true;
    obj.title = "HORIZON CHAMPION FREE ROAM";
    obj.detail = "EXPLORE COLORADO  •  SHOWCASES, BARN FINDS & STREET RACES";
    obj.gps_label = "HORIZON FESTIVAL";
    return obj;
  }

  const int target_idx = std::clamp(collected_wristband + 1, 0, int(bands.size()) - 1);
  const int need = std::max(0, bands[size_t(target_idx)].xp_threshold - wristband_points);
  obj.valid = true;
  obj.title = std::string("EARN ") + bands[size_t(target_idx)].display_name;
  obj.detail = std::to_string(need) + " PTS NEEDED  •  POPULARITY RANK #" + std::to_string(std::clamp(pop_rank, 1, 250));
  obj.gps_label = "NEXT FESTIVAL EVENT";
  return obj;
}

}  // namespace race
