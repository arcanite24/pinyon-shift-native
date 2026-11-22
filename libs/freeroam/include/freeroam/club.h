// The 1000 Club's challenges (SPEC-V4-CONTENT, The 1000 Club): the
// challenge files' grammar parsed into trees, the named locations, localized
// carchallenge* string-table formatting, save_progress persistence, and an
// evaluator that runs all 1,145 challenges across all 218 cars on per-step
// car and game-event signals.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fh1 {
class StringTables;
}

namespace freeroam::club {

enum class Cmp { kEqual, kNotEqual, kMore, kMoreEqual, kLess, kLessEqual };

enum class Cond {
  kSpeed,
  kTimer,
  kGear,
  kInAir,
  kIsDrifting,
  kDistance,
  kLocation,
  kCollided,
  kSkills,
  kSkillPoints,
  kNumSkillChains,
  kSmashables,
  kSpeedCamera,
  kEventStarted,
  kEventRestarted,
  kEventCompleted,
  kEventId,
  kEventHub,
  kCareerEventStyle,
  kEncounterStarted,
  kEncounterCompleted,
  kRaceProgress,
  kUnsupported,
};

struct Condition {
  Cond kind = Cond::kUnsupported;
  Cmp cmp = Cmp::kEqual;
  double value = 0;
  bool inside = true;         // location: mode inside (outside: false)
  std::string name;           // location/skill/smashable/camera/hub/style; unsupported: tag name
  int min_grade = 3;          // skills: 0=Ultimate, 1=Awesome, 2=Great, 3=Standard (inclusive upper bound)
  int max_grade = 0;          // skills: 0=Ultimate .. 3=Standard (inclusive lower bound)
  bool camera_zone = false;   // speed_camera: false=trap, true=zone
  int camera_id = 0;          // speed_camera: trailing _<scoreboard_id> when named
  double min_speed = 0;       // speed_camera: mph (0 = unset)
  double max_speed = 0;       // speed_camera: mph (0 = unset)
  bool unique = false;        // speed_camera: distinct cameras only
  int min_finish_place = 1;   // event_completed / encounter_completed
  std::string mode;           // race_progress: e.g. "furthest"
};

enum class Kind { kPass, kFail, kFailUnless, kAnd, kThen, kOr, kAToB };

struct Node {
  Kind kind = Kind::kPass;
  int pass_count = 1;
  double pass_interval = 0;  // s; 0: none
  bool save_progress = false;
  std::vector<Condition> conditions;  // kPass, kFail, kFailUnless
  std::vector<Node> children;         // groups and a_to_b
  std::string start_loc, end_loc;     // a_to_b
};

struct DescParam {
  enum class Kind { kNone, kNumber, kSkill, kLocation, kSpeedCamera, kEventId };
  Kind kind = Kind::kNone;
  std::string raw;       // attribute value (lowercase for ids/names)
  double number = 0;
  int64_t event_id = 0;
  int min_grade = 3;
  int max_grade = 0;
};

struct Challenge {
  std::string title, description;  // string ids (IDS_...)
  std::string hint0, hint1;        // optional hint string ids
  std::array<DescParam, 5> desc_params{};
  uint32_t definition_hash = 0;    // deterministic FNV-1a hash of the parsed challenge tree
  Node root;
  bool supported = true;
  std::string unsupported;         // first unsupported tag/attr if any

  // Fallback words from title ID when string tables are not loaded.
  std::string label() const;
  // Compact summary of targets when string tables are not loaded.
  std::string summary() const;

  // Localized title / description using FH1 v4 carchallenge* string tables
  // (`carchallengetitles`, `carchallengedescriptions`, `carchallengetransforms`,
  // `carchallenges`), substituting `{0}`..`{4}` from `desc_params`.
  // `strings` and `event_names` may be null.
  std::string format_title(const fh1::StringTables* strings = nullptr) const;
  std::string format_description(const fh1::StringTables* strings = nullptr,
                                 const std::map<int64_t, std::string>* event_names = nullptr) const;
  std::string format_hint(int index, const fh1::StringTables* strings = nullptr) const;
};

// A carchallenges_<car>.xml file: the challenges in file order.
bool parse_challenges(std::string_view xml, std::vector<Challenge>& out, std::string* error);

// named_transforms.xml: each name's points (game coordinates) and radii.
struct Location {
  std::vector<std::array<double, 3>> points;
  std::vector<double> radius;
  bool inside(double x, double z) const;
  bool inside_first(double x, double z) const;
};
inline constexpr double kDefaultRadius = 10.0;  // m
using Locations = std::map<std::string, Location>;
bool parse_named_transforms(std::string_view xml, Locations& out, std::string* error);

// Classifies a SmashableWorld prop stem and PhysicsDefinitions object_type_index
// into one of the 8 1000 Club smashable categories ("sign", "fence", "pole",
// "camera", "picnic", "cone", "flower", "bin") or "generic".
std::string classify_smashable_type(std::string_view prop_stem, int8_t object_type_index);

// Maps SkillTracker's 1..4 grade (1=Standard, 2=Great, 3=Awesome, 4=Ultimate)
// to 1000 Club XML grade (0=Ultimate, 1=Awesome, 2=Great, 3=Standard).
int skill_tracker_grade_to_club_grade(int tracker_grade);

// The installed feature: the package's challenge files (every car's,
// by Data_Car id), the package's folder name under 00000002 (for the
// ownership test), and v4's named locations.
struct ClubData {
  std::string package;
  std::map<int64_t, std::vector<Challenge>> cars;
  Locations locations;
  size_t challenges() const;
  size_t measured() const;
};
// `state_root`: the launcher's state root (user/.../00000002/<package>/
// Media/DLCZips/<offer>/Media/carchallenges/ and title-update-v4/media.zip).
bool load_club(const std::string& state_root, ClubData& out, std::string* error);

// Per-step awarded skill (from SkillTracker).
struct SkillAward {
  std::string type;  // canonical lowercase (e.g. "drift", "nearmiss", "burnout", ...)
  int grade = 3;     // 0=Ultimate, 1=Awesome, 2=Great, 3=Standard
};

// Per-step speed camera hit (from SpeedActivityTracker).
struct SpeedCameraHit {
  bool is_zone = false;   // false=trap, true=zone
  int scoreboard_id = 0;  // 201..230 (trap) or 101..112 (zone)
  std::string name;       // optional camera id/name
  double speed_mph = 0;
};

// The car's state and game-event pulses for one simulation step.
struct Signal {
  double time = 0;        // s
  double speed_mph = 0;   // car's speed
  int gear = 1;           // 0 reverse, 1.. forward (neutral -1)
  bool in_air = false;
  bool is_drifting = false;
  double jump_m = 0;      // current jump's horizontal length so far (0 on ground)
  bool collided = false;  // serious collision this step
  double x = 0, z = 0;    // position, game coordinates

  // Skills & skill chains (Slice 6 SkillTracker):
  std::vector<SkillAward> skills_awarded;
  double skill_points = 0;           // active chain score (chain_points * multiplier)
  bool skill_chain_completed = false;  // pulse when a skill chain banks or breaks

  // Smashables broken this step (Slice 4 SmashableWorld + discount signs):
  std::vector<std::string> smashables_broken;

  // Speed traps & speed zones triggered this step (Slice 6 SpeedActivityTracker):
  std::vector<SpeedCameraHit> speed_cameras;

  // Career race / event state & pulses:
  bool event_started = false;
  bool event_restarted = false;
  bool event_completed = false;
  int event_finish_place = 0;       // 1..N when event_completed is true
  int64_t event_id = 0;             // current active Events.Id (0 = free roam)
  std::string event_hub;            // lowercase: "gladstone", "carson", "redrock", or ""
  std::string career_event_style;   // lowercase: "none", "street", "festival"
  double race_progress = -1.0;      // furthest race progress [0..1]; < 0 before leaving start line

  // Head-to-Head free-roam encounter pulses (Slice 6 FestivalManager):
  bool encounter_started = false;
  bool encounter_completed = false;
  int encounter_finish_place = 0;   // 1 = win, 2 = loss
};

// Metadata per car used to tally the 10 1000 Club medal achievements (SPA 216..225).
struct CarMeta {
  int64_t car_id = 0;
  std::string make;     // display or lowercase make name
  std::string country;  // country name or code (e.g. "UK", "United Kingdom", "Great Britain")
  int64_t base_cost = 0;
};

struct ClubMedalStats {
  int total_medals = 0;
  int cars_with_medal = 0;
  int ford_cars = 0;
  int ferrari_cars = 0;
  int shelby_cars = 0;
  int lamborghini_cars = 0;
  int nissan_cars = 0;
  int bmw_cars = 0;
  int british_cars = 0;
  int million_cr_cars = 0;
  // Individual achievement booleans (SPA 216..225).
  bool ach_first_medal = false;
  bool ach_multi_cars = false;      // >= 5 different cars
  bool ach_ford = false;            // >= 5 Fords
  bool ach_ferrari = false;         // >= 3 Ferraris
  bool ach_shelby = false;          // >= 2 Shelbys
  bool ach_lamborghini = false;     // >= 3 Lamborghinis
  bool ach_nissan = false;          // >= 3 Nissans
  bool ach_bmw = false;             // >= 3 BMWs
  bool ach_british = false;         // >= 5 British cars
  bool ach_million_cr = false;      // >= 1 car worth >= 1,000,000 CR
  int achievements_unlocked = 0;    // 0..10
};

ClubMedalStats compute_medal_stats(const std::vector<std::pair<int64_t, int>>& medals_per_car,
                                   const std::map<int64_t, CarMeta>& meta);

// One challenge's attempt. step() returns true on the step it passes; a
// failure restarts the attempt (its counts too, but save_progress counts
// survive) and is reported through failures().
class Run {
 public:
  Run(const Challenge& challenge, const Locations& locations);
  bool step(const Signal& s);
  bool passed() const { return passed_; }
  int failures() const { return failures_; }  // attempts that had made progress
  // An attempt that has got somewhere (a trip started, a step passed, a
  // held condition for over a second): the HUD shows it.
  bool under_way(const Signal& s) const;
  // What the attempt is waiting for (compact HUD status text).
  std::string status(const Signal& s) const;
  // [0..1] completion progress for HUD / pause screen progress bars.
  double progress(const Signal& s) const;

  // Persistence for `save_progress="true"` nodes (preorder counts).
  std::vector<int> export_save_progress() const;
  void import_save_progress(const std::vector<int>& counts);
  bool has_save_progress() const;

 private:
  enum class Result { kRunning, kPassed, kFailed };
  struct CameraSample {
    double time = 0;
    std::string key;
    double speed_mph = 0;
  };
  struct State {
    bool active = false, done = false;
    double start = 0;
    int count = 0, current = 0;  // pass count; a then-group's current child
    int chains = 0;              // skill chains completed since activation
    bool was_true = false;
    bool started = false;        // a_to_b: left A
    std::deque<double> times;    // pass_interval timestamps
    std::deque<CameraSample> camera_hits;  // combined speed or timed speed_camera hits
    std::set<std::string> seen_cameras;    // unique speed_camera keys in current attempt
    std::vector<State> children;
  };
  void build(const Node& n, State& st);
  void activate(const Node& n, State& st, double t, bool keep_saved);
  Result advance(const Node& n, State& st, const Signal& s);
  bool holds(const Condition& c, const State& owner, const Signal& s) const;
  bool progressed(const Node& n, const State& st) const;
  void export_saved_rec(const Node& n, const State& st, std::vector<int>& out) const;
  void import_saved_rec(const Node& n, State& st, const std::vector<int>& in, size_t& idx);
  static bool has_saved_rec(const Node& n);

  const Challenge* c_;
  const Locations* locs_;
  State root_;
  bool passed_ = false, unsupported_ = false;
  int failures_ = 0;
};

}  // namespace freeroam::club
