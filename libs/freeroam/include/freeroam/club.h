// The 1000 Club's challenges (SPEC-V4-CONTENT, The 1000 Club): the
// challenge files' grammar parsed into trees, the named locations, and an
// evaluator that runs one challenge on a per-step signal of the car.
//
// Only the conditions the engine measures today are evaluated: speed,
// timer, gear, in_air, distance (the current jump), location and collided.
// A challenge using any other condition (skills, speed cameras, events,
// smashables, encounters, skill points) is unsupported as a whole: it never
// starts, so it can never pass on a partial reading.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace freeroam::club {

enum class Cmp { kEqual, kNotEqual, kMore, kMoreEqual, kLess, kLessEqual };

enum class Cond { kSpeed, kTimer, kGear, kInAir, kDistance, kLocation, kCollided, kUnsupported };

struct Condition {
  Cond kind = Cond::kUnsupported;
  Cmp cmp = Cmp::kEqual;
  double value = 0;
  bool inside = true;  // location: mode inside (outside: false)
  std::string name;    // location: loc_name; unsupported: the element name
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

struct Challenge {
  std::string title, description;  // string ids (IDS_...)
  Node root;
  bool supported = true;
  std::string unsupported;  // the first condition the engine does not measure
  // Ours, for the HUD until the string tables are read: the title id as
  // words and a summary of the targets.
  std::string label() const;
  std::string summary() const;
};

// A carchallenges_<car>.xml file: the challenges in file order.
bool parse_challenges(std::string_view xml, std::vector<Challenge>& out, std::string* error);

// named_transforms.xml: each name's points (game coordinates) and radii.
// Interim (spec): a location is the union of its circles on the
// horizontal plane; a point without a radius gets kDefaultRadius.
struct Location {
  std::vector<std::array<double, 3>> points;
  std::vector<double> radius;
  bool inside(double x, double z) const;
  bool inside_first(double x, double z) const;
};
inline constexpr double kDefaultRadius = 10.0;  // m (ours)
using Locations = std::map<std::string, Location>;
bool parse_named_transforms(std::string_view xml, Locations& out, std::string* error);

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

// The car's state for one simulation step.
struct Signal {
  double time = 0;       // s
  double speed_mph = 0;  // the car's speed
  int gear = 1;          // the challenge's count: 0 reverse, 1.. forward (neutral -1)
  bool in_air = false;
  double jump_m = 0;     // the current jump's horizontal length so far (0 on the ground)
  bool collided = false; // a serious collision this step
  double x = 0, z = 0;   // position, game coordinates
};

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
  // Ours, for the HUD: what the attempt is waiting for.
  std::string status(const Signal& s) const;

 private:
  enum class Result { kRunning, kPassed, kFailed };
  struct State {
    bool active = false, done = false;
    double start = 0;
    int count = 0, current = 0;  // pass count; a then-group's current child
    bool was_true = false;
    bool started = false;  // a_to_b: left A
    std::deque<double> times;  // pass_interval
    std::vector<State> children;
  };
  void build(const Node& n, State& st);
  void activate(const Node& n, State& st, double t, bool keep_saved);
  Result advance(const Node& n, State& st, const Signal& s);
  bool holds(const Condition& c, const State& owner, const Signal& s) const;
  bool progressed(const Node& n, const State& st) const;

  const Challenge* c_;
  const Locations* locs_;
  State root_;
  bool passed_ = false, unsupported_ = false;
  int failures_ = 0;
};

}  // namespace freeroam::club
