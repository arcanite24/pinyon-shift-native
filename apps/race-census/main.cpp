// race-census <game_root>: every event's race against what carview --event
// can run (SPEC-EVENTS, Race type and Data chain): the route file, its
// slots, start line and sprint finish, the AI line and its closed flag.
// One line an event, then counts by career type. Exit 1 if a supported
// race (an AI line, ribbon 0 closed or ribbon 2 open, one lap) fails.
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "race/event.h"
#include "race/owt.h"
#include "race/route.h"
#include "race/street_line.h"
#include "traffic/nav.h"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: race-census <game_root> [--quiet]\n");
    return 2;
  }
  const std::filesystem::path root = argv[1];
  // --street-line <route>: the street line's points as CSV (x, y, z), for
  // comparing against an oracle path.
  if (argc > 3 && std::string(argv[2]) == "--street-line") {
    race::Route route;
    traffic::NavMap nav;
    race::AiLineFile line;
    std::string err;
    if (!race::load_route(root, std::atoi(argv[3]), route, &err) ||
        !traffic::read_nav(root / "media" / "tracks" / "colorado" / "colorado.nav", nav, &err) ||
        !race::build_street_line(nav, route, line, &err)) {
      std::fprintf(stderr, "race-census: %s\n", err.c_str());
      return 1;
    }
    std::printf("x,y,z\n");
    for (const auto& p : line.points) std::printf("%.2f,%.2f,%.2f\n", p.x, p.y, p.z);
    return 0;
  }
  const bool quiet = argc > 2 && std::string(argv[2]) == "--quiet";
  std::vector<race::EventSummary> events;
  std::string e;
  if (!race::list_events(root, events, &e)) {
    std::fprintf(stderr, "race-census: %s\n", e.c_str());
    return 1;
  }
  struct Count {
    int races = 0, runnable = 0;
  };
  std::map<int, Count> by_type;
  int failures = 0, runnable = 0;
  for (const auto& ev : events) {
    race::Route route;
    race::AiLineFile owt;
    std::string why;
    const bool have_route = race::load_route(root, ev.route_id, route, &why);
    const bool have_line = race::load_owt(root, ev.route_id, owt, nullptr);
    const size_t slots = have_route ? route.slots().size() : 0;
    const bool gate = have_route && route.start_line().has_value();
    const bool finish = have_route && route.sprint_finish().has_value();
    // What carview runs today: an AI line; a closed line is a circuit (any
    // laps), an open one a one-lap sprint with a finish.
    const char* verdict = "ok";
    const bool street = have_route && !have_line && !race::checkpoints(route).empty();
    if (!have_route) verdict = "no route";
    else if (street) verdict = slots ? "ok" : "street, no slots";  // a line along the road map
    else if (!have_line) verdict = "no AI line";
    else if (slots == 0) verdict = "no slots";
    else if (!gate && owt.closed) verdict = "no start line";
    else if (!owt.closed && !finish && !route.find("route_waypoint_00")) verdict = "open line, no finish";
    else if (!owt.closed && ev.laps > 1) verdict = "open line, laps > 1";
    const bool ok = std::string(verdict) == "ok";
    const bool supported = street || (have_line && ((owt.closed && ev.ribbon == 0) || (!owt.closed && ev.laps == 1)));
    if (supported && !ok) ++failures;
    runnable += ok;
    auto& c = by_type[ev.career_type];
    ++c.races;
    c.runnable += ok;
    if (!quiet)
      std::printf("%-6s type %2d ribbon %2d route %3d laps %d opp %d slots %2zu line %-6s gate %d finish %d  %s\n",
                  ev.horizon_id.c_str(), ev.career_type, ev.ribbon, ev.route_id, ev.laps, ev.opponents, slots,
                  have_line ? (owt.closed ? "closed" : "open") : "-", int(gate), int(finish), verdict);
  }
  std::printf("race-census: %zu races, %d runnable\n", events.size(), runnable);
  for (const auto& [t, c] : by_type) std::printf("  career type %2d: %3d races, %3d runnable\n", t, c.races, c.runnable);
  if (failures) std::printf("race-census: %d supported races fail\n", failures);
  return failures ? 1 : 0;
}
