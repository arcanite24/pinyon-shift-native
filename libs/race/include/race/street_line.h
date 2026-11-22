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

// The plane of checkpoint k (SPEC-EVENTS, Pass rule): through its position,
// normal along its facing (from checkpoint k - 1, or slot 00, when it has
// none), crossed either way within half its width (25 m when unset).
std::optional<Gate> checkpoint_gate(const Route& route, size_t k);

struct StreetLineOptions {
  double half_width = 4.5;  // m, the AI's measured spread about the road's centre (SPEC-RACE-AI)
  double spacing = 4.0;     // m between the line's points
  int smooth_passes = 3;    // corner rounding (see build_street_line)
  // The racing offset (SPEC-RACE-AI, Routes with no AI line: "to the
  // inside of bends"; the amount is ours): a minimum-curvature relaxation
  // (relax_passes per chord) within `max_offset` half widths.
  int relax_passes = 60;
  double max_offset = 0.51;
};

// The line from slot 00 through every checkpoint along the road map; false
// with `error` when a checkpoint has no road near it or no path joins two.
bool build_street_line(const traffic::NavMap& nav, const Route& route, AiLineFile& out, std::string* error,
                       const StreetLineOptions& o = {});

}  // namespace race
