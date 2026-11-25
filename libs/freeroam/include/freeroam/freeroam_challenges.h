// PROG-11: FH1's free-roam sponsor challenges (media/gametunablesettings.zip
// FreeRoamChallenges.xml, text from stringtables/<lang>.zip
// FreeRoamChallenges.str). The original runs them as Playground co-op
// challenges ("CO-OP CHALLENGES", one at a time, chosen by the session's
// host); the engine has no online sessions, so it tracks them solo.
//
// The shipped file holds 24 live challenges: 1 DistanceDriven, 14
// DriveFromAToB, 3 Skills and 6 SpeedCamera. A further 19 DistanceDriven
// ones ("Drive75Miles*", 25,000 CR each) sit in an XML comment ("Old
// distance challenges") and have no strings; they are parsed as `retired`
// so the whole authored set (43) is visible, but are never tracked or paid.
//
// Rules, as the original applies them:
//  - Every restriction must hold (an AND over the list) for a step to count.
//  - TimeOfDayRestriction: allowed in [start, end), wrapping past midnight.
//  - SpeedRestriction: mph, speed >= min_speed and < max_speed; 0 = unset.
//  - CarRestriction: every given attribute must match (ids exactly, *Min /
//    *Max inclusive); empty attributes are ignored.
//  - DistanceDriven: the challenge's own accumulator (not the profile's
//    total distance) gains the distance moved while the restrictions hold;
//    complete at distance_to_drive metres.
//  - Skills: one count per award of feat_id (any grade) while the
//    restrictions hold; complete at target_feat_count.
//  - SpeedCamera: each player's latest flash per listed camera; complete
//    when the local player flashed and the players who flashed within
//    time_limit seconds reach num_cameras_to_flash (with
//    cameras_must_be_unique, distinct cameras count instead of players).
//    The counts are 2/4/8, so a solo player never completes one.
//  - DriveFromAToB: min_players (2/4/8) must make the trip; never completes
//    solo.
// Each challenge completes once and pays its Reward credits.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace fh1 {
struct StringTable;
}

namespace freeroam {

enum class FreeRoamChallengeType : uint8_t { kUnknown, kDistanceDriven, kDriveFromAToB, kSkills, kSpeedCamera };

// CarRestriction attributes (the XML's names, in its order).
enum class CarField : uint8_t {
  kClass,  // CarClassId 0-10 (0=F, 10=U)
  kPi,
  kPower,
  kWeight,
  kYear,
  kBodyFamily,
  kCarType,
  kRegion,
  kCountry,
  kMake,
  kModelFamily,
  kModel,
  kDriveType,
  kEngineConfig,
  kEnginePlacement,
  kCylinder,
  kAspiration,
  kFamilySpecial,
  kBaseCost,
  kCount
};

// The car being driven; a negative value is unknown and fails any
// restriction on that attribute.
struct ChallengeCar {
  std::array<double, size_t(CarField::kCount)> value;
  bool upgraded = false;
  ChallengeCar() { value.fill(-1.0); }
  double& operator[](CarField f) { return value[size_t(f)]; }
  double operator[](CarField f) const { return value[size_t(f)]; }
};

struct ChallengeRestriction {
  enum class Kind : uint8_t { kUnknown, kTimeOfDay, kSpeed, kCar };
  Kind kind = Kind::kUnknown;
  std::string type;  // the XML's type attribute
  // TimeOfDay, in hours [0, 24).
  double start_hour = 0, end_hour = 0;
  // Speed, mph; 0 = no bound.
  double min_mph = 0, max_mph = 0;
  // Car: each attribute's inclusive range (an id has min == max).
  struct Range {
    CarField field = CarField::kMake;
    double min = 0, max = 0;
  };
  std::vector<Range> car;
  int allow_upgrades = -1;  // -1 unset, 0 stock only, 1 allowed

  bool holds(double tod_hour, double speed_mph, const ChallengeCar& car) const;
};

struct FreeRoamChallengeDef {
  std::string id;
  std::string type_name;
  FreeRoamChallengeType type = FreeRoamChallengeType::kUnknown;
  bool retired = false;  // authored inside an XML comment: not live
  std::string sponsor_name_key, challenge_name_key, description_key, sponsor_logo;
  // Text from FreeRoamChallenges.str (empty when the key is missing).
  std::string sponsor_name, challenge_name, description;
  int64_t reward_credits = 0;
  // DistanceDriven.
  double distance_to_drive_m = 0;
  // Skills.
  std::string feat_id;
  int target_feat_count = 0;
  // DriveFromAToB.
  std::array<double, 3> start_location{}, end_location{};
  double start_radius = 0, end_radius = 0;
  int min_players = 1;
  bool bidirectional = false;
  // DriveFromAToB and SpeedCamera, s (0 = none).
  double time_limit_s = 0;
  // SpeedCamera.
  int num_cameras_to_flash = 0;
  bool cameras_must_be_unique = false;
  std::vector<std::string> camera_ids;
  std::vector<ChallengeRestriction> restrictions;

  // "SPONSORLOGOTILE/<FILE>" for the HUD's sponsor logo, empty without one.
  std::string logo_texture() const;
  bool restrictions_hold(double tod_hour, double speed_mph, const ChallengeCar& car) const;
};

struct FreeRoamChallengesData {
  std::vector<FreeRoamChallengeDef> challenges;  // live ones first, then retired
  const FreeRoamChallengeDef* find(std::string_view id) const;
  size_t live_count() const;
};

// Parses FreeRoamChallenges.xml; `strings` (optional) resolves the IDS_ keys.
bool parse_free_roam_challenges(std::string_view xml, FreeRoamChallengesData& out, std::string* error = nullptr,
                                const fh1::StringTable* strings = nullptr);
// Loads `<game_root>/media/gametunablesettings.zip` FreeRoamChallenges.xml and
// (when present) `media/stringtables/EN.zip` FreeRoamChallenges.str.
bool load_free_roam_challenges(const std::filesystem::path& game_root, FreeRoamChallengesData& out,
                               std::string* error = nullptr);

// A speed camera or speed zone result. `player` 0 is the local player; other
// ids are session players (none offline).
struct ChallengeCameraFlash {
  std::string camera_id;
  double speed_mph = 0;
  int player = 0;
};

// One simulation step's inputs.
struct ChallengeStepInput {
  double time_s = 0;      // session clock
  double distance_m = 0;  // moved this step
  double speed_mph = 0;
  double tod_hour = 12;   // [0, 24)
  ChallengeCar car;
  std::vector<std::string> skills;  // HorizonFeats ids awarded this step
  std::vector<ChallengeCameraFlash> flashes;
};

struct ChallengeCompletion {
  std::string id;
  int64_t credits = 0;
  const FreeRoamChallengeDef* def = nullptr;
};

// A challenge's saved state: the distance (m) or skill count, and whether it
// completed.
struct ChallengeRecord {
  std::string id;
  double progress = 0;
  bool completed = false;
};

class FreeRoamChallengeTracker {
 public:
  explicit FreeRoamChallengeTracker(const FreeRoamChallengesData& data);

  // The co-op challenge the host set; empty tracks every live one (the
  // engine's solo default: interim, the original tracks only the set one).
  void set_active(std::string id) { active_ = std::move(id); }
  const std::string& active() const { return active_; }

  std::vector<ChallengeCompletion> step(const ChallengeStepInput& in);

  bool completed(std::string_view id) const;
  double progress(std::string_view id) const;  // distance (m) or skill count
  // 0..1 towards completion (1 once completed).
  double fraction(std::string_view id) const;
  int completed_count() const;

  std::vector<ChallengeRecord> save() const;
  void load(const std::vector<ChallengeRecord>& records);

 private:
  struct State {
    double progress = 0;
    bool completed = false;
    // SpeedCamera: camera id (lower case) -> player -> latest flash time.
    std::map<std::string, std::map<int, double>> flashes;
  };
  bool tracked(const FreeRoamChallengeDef& d) const;
  const FreeRoamChallengesData& data_;
  std::map<std::string, State> states_;  // by challenge id
  std::string active_;
};

}  // namespace freeroam
