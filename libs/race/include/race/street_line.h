// Street and nemesis races (SPEC-EVENTS: ribbon 6, checkpoints, no AI
// line; SPEC_QUESTIONS Q47 open). Ours meanwhile: the route's
// route_checkpoint_NN transforms in order are the race's gates; the AI line
// is the shortest path on the road map (colorado.nav) from slot 00 through
// every checkpoint, as an open AI line file (centre, a lateral of the
// road's assumed half width, no racing offset), so the rest of the race
// (racing line, ribbon ground, places) runs as on a festival sprint.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "race/owt.h"
#include "race/route.h"
#include "traffic/nav.h"

namespace race {

// route_checkpoint_00, _01, ... in order, until one is missing.
std::vector<const RouteTransform*> checkpoints(const Route& route);

// The plane of checkpoint k, normal from checkpoint k - 1 to k (slot 00 for
// the first), counted within its width (or 30 m) of the point.
std::optional<Gate> checkpoint_gate(const Route& route, size_t k);

struct StreetLineOptions {
  double half_width = 5.0;  // m, the road's assumed half width (the nav map has none)
  double spacing = 4.0;     // m between the line's points
  int smooth_passes = 3;    // corner rounding (see build_street_line)
};

// The line from slot 00 through every checkpoint along the road map; false
// with `error` when a checkpoint has no road near it or no path joins two.
bool build_street_line(const traffic::NavMap& nav, const Route& route, AiLineFile& out, std::string* error,
                       const StreetLineOptions& o = {});

}  // namespace race
