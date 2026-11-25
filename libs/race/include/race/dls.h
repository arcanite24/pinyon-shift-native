// SPEC-RACE-AI, Traffic between cars: the AI's passing search, the
// dynamic-line search (DLS) of AIRacing.xml's <DLS> and <PassingDriver>
// blocks. Each AI car, at most every TimeBetweenDynamicLineUpdates, picks
// the car it will reach first, scores 201 lateral slots 0.5 m apart around
// its own predicted place at that meeting, and moves its lane to the
// cheapest slot when the cost is acceptable:
//
//   cost = OutOfBounds [past the road edge] + OffTrack [on the edge slot]
//        + Car [covered by a car] + |offset| max(1, v OffLine / max(d, 2))
//        + TimeToCollision (g(k) + (g(k-1) + g(k+1)) / 2),
//          g = max(H - the slot's time to a car, 0), H = 2.25 s
//        + ChangePassSides [the other side of the target than the pass so far]
//        + HeadOnPassWrongSide [the wrong side of an oncoming target, by
//          LegalSideOfRoad]
//
// A failed search keeps the car where it is and adds 5 to its frustration;
// past 1000 it stops trying and follows (trailing the car at 0.75 x the
// temperament's TrailingDistance). Interim parts (Q123): the predictions
// run along the shared line at constant speed and lateral place; the
// return to the line, the lane's transition shape and the trailing speed
// cap are ours.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "drive/ai_driver.h"

namespace race {

// AIRacing.xml <DLS> and <PassingDriver> of one settings set (the values in
// the comments are the `default` set's).
struct DlsSettings {
  double time_to_collision_cost = 160;  // TimeToCollsionCost (sic)
  double out_of_bounds_cost = 400;      // OutOfBoundsCost
  double off_track_cost = 400;          // OffTrackCost
  double car_cost = 400;                // CarCost
  double off_line_cost = 3.3;           // OffLineCost
  double change_pass_sides_cost = 60;   // ChangePassSidesCost
  double head_on_wrong_side_cost = 0;   // HeadOnPassWrongSideCost
  double max_acceptable_cost = 400;     // MaxAcceptableCost
  double extra_width_ai = 0.2, extra_width_human = 0.3, extra_width_traffic = 0.35;  // ExtraCarWidth*
  double update_interval = 0.2;          // PassingDriver TimeBetweenDynamicLineUpdates (s)
  bool ignore_cars_not_approached = true;  // PassingDriver ShouldIgnoreCarsNotApproached
  int legal_side = 0;                    // PassingDriver LegalSideOfRoad: 0 None, 1 Left, 2 Right
};

// One car as the search sees it, in the shared line's frame.
struct DlsCar {
  enum class Kind : uint8_t { kAi, kHuman, kTraffic };
  double progress = 0;  // m along the line
  float cross = 0;      // m right of the line
  float speed = 0;      // m/s along the line (negative: oncoming)
  float half_width = 1.0f, half_length = 2.3f;  // m
  Kind kind = Kind::kAi;
};

// An AI car's passing state (kept in the rewind snapshot).
struct DlsState {
  float timer = 0;        // s until the next search
  float target_lane = 0;  // m right of the line the lane moves to
  float rate = 0;         // m of lane per m travelled towards target_lane
  int32_t target = -1;    // the car being passed (index into the cars), -1 none
  uint8_t side = 0;       // the pass side of the target: 0 none, 1 its left, 2 its right
  bool following = false; // frustrated: trailing the target instead
  float frustration = 0;
};

// The temperament's passing parameters (AITemperaments).
struct LaneParams {
  float trailing = 7, car_clearance = 0.3f, edge_clearance = 0.4f;
  float aggression = 0.5f;  // Passing_AggressionLevel
};

// A pair's prediction over the horizon: the first time the two boxes
// touch, else the closest approach (within 32 m).
struct DlsMeeting {
  bool valid = false, hit = false;
  double t = 0;         // s
  double miss = 1e9;    // m between the boxes at the closest approach (0 on a hit)
};
// `along_only`: as if both were at the same lateral place (the meeting a
// car keeps with its target until it is past it).
DlsMeeting dls_predict(const DlsCar& self, float self_lane, const DlsCar& other, double horizon = 2.25,
                       bool along_only = false);

// The slot search for car `i` against its target `target` (index into
// `cars`): fills `lane` with the chosen lane (m right of the line) and
// `side`, and returns the cost; above settings.max_acceptable_cost means
// failure (lane then is the car's own place).
double dls_search(const drive::RacingLine& line, std::span<const DlsCar> cars, size_t i, float self_lane,
                  size_t target, const DlsMeeting& meet, uint8_t current_side, const DlsSettings& settings,
                  float& lane, uint8_t& side);

// One step for the AI cars [0, ai): picks targets, runs the search when
// due, moves each lane (m right of the line) and sets each speed cap
// (m/s, 1e9: none).
void dls_update(const drive::RacingLine& line, std::span<const DlsCar> cars, size_t ai,
                std::span<const LaneParams> params, const DlsSettings& settings, double dt, std::span<DlsState> state,
                std::span<float> lane, std::span<float> cap);

}  // namespace race
