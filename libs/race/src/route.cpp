#include "race/route.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace race {
namespace {

// The attributes of the tag starting at `at` (just past its name), up to
// its '>': name='value' or name="value".
std::vector<std::pair<std::string_view, std::string_view>> attributes(std::string_view s, size_t at, size_t& end) {
  std::vector<std::pair<std::string_view, std::string_view>> out;
  size_t i = at;
  while (i < s.size() && s[i] != '>') {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n' || s[i] == '/')) ++i;
    if (i >= s.size() || s[i] == '>') break;
    const size_t n0 = i;
    while (i < s.size() && s[i] != '=' && s[i] != '>' && s[i] != ' ') ++i;
    const std::string_view name = s.substr(n0, i - n0);
    if (i >= s.size() || s[i] != '=') continue;
    ++i;
    if (i >= s.size() || (s[i] != '\'' && s[i] != '"')) continue;
    const char q = s[i++];
    const size_t v0 = i;
    while (i < s.size() && s[i] != q) ++i;
    out.emplace_back(name, s.substr(v0, i - v0));
    if (i < s.size()) ++i;
  }
  end = i;
  return out;
}

double number(std::string_view v) { return std::strtod(std::string(v).c_str(), nullptr); }

}  // namespace

double Gate::crossing(double x0, double z0, double x1, double z1) const {
  const double a = side(x0, z0), b = side(x1, z1);
  if (!(a < 0 && b >= 0)) return -1;
  const double f = a / (a - b);
  const double cx = x0 + f * (x1 - x0), cz = z0 + f * (z1 - z0);
  if ((cx - x) * (cx - x) + (cz - z) * (cz - z) > reach * reach) return -1;
  return f;
}

const RouteTransform* Route::find(std::string_view name) const {
  for (const auto& t : transforms)
    if (t.name == name) return &t;
  return nullptr;
}

std::vector<const RouteTransform*> Route::slots() const {
  std::vector<const RouteTransform*> out;
  for (int k = 0; k < 100; ++k) {
    char n[32];
    std::snprintf(n, sizeof n, "start_location_%02d", k);
    const RouteTransform* t = find(n);
    if (!t) break;
    out.push_back(t);
  }
  return out;
}

std::optional<Gate> Route::start_line() const {
  const RouteTransform* g = find("start_gantry_cannon_00");
  const auto s = slots();
  if (!g || s.empty()) return std::nullopt;
  const double l = std::hypot(s[0]->fx, s[0]->fz);
  if (l < 1e-9) return std::nullopt;
  Gate gate{g->x, g->z, s[0]->fx / l, s[0]->fz / l, 60.0};
  return gate;
}

std::optional<Gate> Route::sprint_finish() const {
  const RouteTransform *a = find("end_race_cannon_left_04"), *b = find("end_race_cannon_right_04");
  if (!a || !b) return std::nullopt;
  double nx = a->fx + b->fx, nz = a->fz + b->fz;
  const double l = std::hypot(nx, nz);
  if (l < 1e-9) return std::nullopt;
  nx /= l, nz /= l;
  Gate gate{0.5 * (a->x + b->x) + 3.5 * nx, 0.5 * (a->z + b->z) + 3.5 * nz, nx, nz};
  return gate;
}

bool parse_route(std::string_view s, Route& out, std::string* error) {
  out.transforms.clear();
  constexpr std::string_view kNamed = "<NamedTransform", kTransform = "<Transform", kClose = "</NamedTransform>";
  size_t at = 0;
  while ((at = s.find(kNamed, at)) != std::string_view::npos) {
    // Not the <NamedTransforms> list itself.
    const size_t after = at + kNamed.size();
    if (after < s.size() && s[after] != ' ' && s[after] != '\t' && s[after] != '\r' && s[after] != '\n') {
      at = after;
      continue;
    }
    size_t end = 0;
    RouteTransform t;
    for (const auto& [k, v] : attributes(s, at + kNamed.size(), end)) {
      if (k == "name") t.name = v;
      else if (k == "width") t.width = number(v);
      else if (k == "tag") t.tag = v;
    }
    const size_t close = s.find(kClose, end);
    const size_t tr = s.find(kTransform, end);
    if (tr == std::string_view::npos || (close != std::string_view::npos && tr > close)) {
      if (error) *error = "route: " + t.name + " has no Transform";
      return false;
    }
    size_t tend = 0;
    for (const auto& [k, v] : attributes(s, tr + kTransform.size(), tend)) {
      if (k == "pos.x") t.x = number(v);
      else if (k == "pos.y") t.y = number(v);
      else if (k == "pos.z") t.z = number(v);
      else if (k == "facing.x") t.fx = number(v);
      else if (k == "facing.z") t.fz = number(v);
      else if (k == "width") t.width = number(v);
      else if (k == "tag") t.tag = v;
    }
    out.transforms.push_back(std::move(t));
    at = close == std::string_view::npos ? tend : close + kClose.size();
  }
  return true;
}

std::filesystem::path route_path(const std::filesystem::path& game_root, int route_id) {
  char n[40];
  std::snprintf(n, sizeof n, "TrackRoute%03d.xml", route_id);
  return game_root / "media" / "tracks" / "colorado" / "Ribbon_00" / n;
}

bool load_route(const std::filesystem::path& game_root, int route_id, Route& out, std::string* error) {
  const auto p = route_path(game_root, route_id);
  std::ifstream in(p, std::ios::binary);
  if (!in) {
    if (error) *error = "cannot read " + p.string();
    return false;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  out.id = route_id;
  return parse_route(ss.str(), out, error);
}

}  // namespace race
