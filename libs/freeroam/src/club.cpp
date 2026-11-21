#include "freeroam/club.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "audio/xml.h"
#include "fh1data/archive.h"

namespace freeroam::club {

namespace {

bool fail(std::string* error, const std::string& what) {
  if (error) *error = what;
  return false;
}

std::string lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool parse_cmp(const std::string& s, Cmp& out) {
  static const std::pair<const char*, Cmp> kNames[] = {
      {"equal_to", Cmp::kEqual},     {"not_equal_to", Cmp::kNotEqual}, {"more_than", Cmp::kMore},
      {"more_equal", Cmp::kMoreEqual}, {"less_than", Cmp::kLess},      {"less_equal", Cmp::kLessEqual}};
  for (const auto& [name, c] : kNames)
    if (audio::iequals(s, name)) return out = c, true;
  return false;
}

const char* cmp_text(Cmp c) {
  switch (c) {
    case Cmp::kEqual: return "=";
    case Cmp::kNotEqual: return "!=";
    case Cmp::kMore: return ">";
    case Cmp::kMoreEqual: return ">=";
    case Cmp::kLess: return "<";
    case Cmp::kLessEqual: return "<=";
  }
  return "?";
}

bool compare(double a, Cmp c, double b) {
  switch (c) {
    case Cmp::kEqual: return a == b;
    case Cmp::kNotEqual: return a != b;
    case Cmp::kMore: return a > b;
    case Cmp::kMoreEqual: return a >= b;
    case Cmp::kLess: return a < b;
    case Cmp::kLessEqual: return a <= b;
  }
  return false;
}

struct Parser {
  Challenge* c;
  void unsupported(const std::string& what) {
    if (c->supported) c->supported = false, c->unsupported = what;
  }
  Condition condition(const audio::XmlNode& e) {
    Condition k;
    k.name = lower(e.name);
    static const std::pair<const char*, Cond> kKinds[] = {
        {"speed", Cond::kSpeed},       {"timer", Cond::kTimer},       {"gear", Cond::kGear},
        {"in_air", Cond::kInAir},      {"distance", Cond::kDistance}, {"location", Cond::kLocation},
        {"collided", Cond::kCollided}};
    for (const auto& [name, kind] : kKinds)
      if (k.name == name) k.kind = kind;
    if (k.kind == Cond::kUnsupported) {
      unsupported(k.name);
      return k;
    }
    if (k.kind == Cond::kSpeed || k.kind == Cond::kTimer || k.kind == Cond::kGear || k.kind == Cond::kDistance) {
      if (!parse_cmp(e.attr("is"), k.cmp) || !e.has("value")) unsupported(k.name + " without is/value");
      k.value = e.number("value");
    }
    if (k.kind == Cond::kLocation) {
      k.inside = !audio::iequals(e.attr("mode", "inside"), "outside");
      k.name = lower(e.attr("loc_name"));
      if (k.name.empty()) unsupported("location without loc_name");
    }
    return k;
  }
  Node node(const audio::XmlNode& e) {
    Node n;
    const std::string name = lower(e.name);
    if (name == "pass_when") n.kind = Kind::kPass;
    else if (name == "fail_when") n.kind = Kind::kFail;
    else if (name == "fail_unless") n.kind = Kind::kFailUnless;
    else if (name == "a_to_b") n.kind = Kind::kAToB;
    else if (name == "group") {
      const std::string type = lower(e.attr("type", "and"));
      n.kind = type == "then" ? Kind::kThen : type == "or" ? Kind::kOr : Kind::kAnd;
      if (type != "then" && type != "or" && type != "and") unsupported("group type " + type);
    } else {
      unsupported(name);
      return n;
    }
    n.pass_count = std::max(1, int(e.number("pass_count", 1)));
    n.pass_interval = e.number("pass_interval", 0);
    n.save_progress = e.has("save_progress") && !audio::iequals(e.attr("save_progress"), "false");
    n.start_loc = lower(e.attr("start_loc"));
    n.end_loc = lower(e.attr("end_loc"));
    for (const auto& k : e.children) {
      if (audio::iequals(k.name, "hud")) continue;
      if (n.kind == Kind::kPass || n.kind == Kind::kFail || n.kind == Kind::kFailUnless)
        n.conditions.push_back(condition(k));
      else
        n.children.push_back(node(k));
    }
    return n;
  }
};

std::string describe(const Node& n) {
  auto cond = [](const Condition& c) {
    char b[96];
    switch (c.kind) {
      case Cond::kSpeed: std::snprintf(b, sizeof b, "speed %s %g mph", cmp_text(c.cmp), c.value); break;
      case Cond::kTimer: std::snprintf(b, sizeof b, "%s %g s", cmp_text(c.cmp), c.value); break;
      case Cond::kGear: std::snprintf(b, sizeof b, "gear %s %g", cmp_text(c.cmp), c.value); break;
      case Cond::kDistance: std::snprintf(b, sizeof b, "jump %s %g m", cmp_text(c.cmp), c.value); break;
      case Cond::kInAir: return std::string("in the air");
      case Cond::kCollided: return std::string("a crash");
      case Cond::kLocation: return std::string(c.inside ? "in " : "out of ") + c.name;
      case Cond::kUnsupported: return c.name;
    }
    return std::string(b);
  };
  auto conds = [&](const Node& k) {
    std::string s;
    for (const auto& c : k.conditions) s += (s.empty() ? "" : " and ") + cond(c);
    return s;
  };
  switch (n.kind) {
    case Kind::kPass: {
      std::string s = conds(n);
      if (n.pass_count > 1) s += " x" + std::to_string(n.pass_count);
      return s;
    }
    case Kind::kFail: return "fails on " + conds(n);
    case Kind::kFailUnless: return "while " + conds(n);
    case Kind::kAToB: {
      std::string s = n.start_loc + " to " + n.end_loc;
      for (const auto& k : n.children) s += ", " + describe(k);
      return s;
    }
    case Kind::kAnd:
    case Kind::kThen:
    case Kind::kOr: {
      std::string s;
      const char* sep = n.kind == Kind::kThen ? ", then " : n.kind == Kind::kOr ? " or " : ", ";
      for (const auto& k : n.children)
        s += (s.empty() ? "" : (k.kind == Kind::kFail || k.kind == Kind::kFailUnless) ? ", " : sep) + describe(k);
      return n.children.size() > 1 && n.kind != Kind::kAnd ? "(" + s + ")" : s;
    }
  }
  return {};
}

bool is_fail(const Node& n) { return n.kind == Kind::kFail || n.kind == Kind::kFailUnless; }

void check_locations(const Node& n, const Locations& locs, std::string& missing) {
  auto need = [&](const std::string& name) {
    if (!name.empty() && !locs.count(name) && missing.empty()) missing = "location " + name;
  };
  if (n.kind == Kind::kAToB) need(n.start_loc), need(n.end_loc);
  for (const auto& c : n.conditions)
    if (c.kind == Cond::kLocation) need(c.name);
  for (const auto& k : n.children) check_locations(k, locs, missing);
}

}  // namespace

std::string Challenge::label() const {
  std::string s = title;
  if (s.rfind("IDS_", 0) == 0) s = s.substr(4);
  s = lower(s);
  for (char& ch : s)
    if (ch == '_' || ch == '-') ch = ' ';
  return s;
}

std::string Challenge::summary() const { return describe(root); }

bool parse_challenges(std::string_view xml, std::vector<Challenge>& out, std::string* error) {
  audio::XmlNode root;
  if (!audio::parse_xml(xml, root, error)) return false;
  if (!audio::iequals(root.name, "car_challenges")) return fail(error, "not a car_challenges file");
  out.clear();
  const auto& kids = root.children;
  for (size_t i = 0; i < kids.size(); ++i) {
    if (!audio::iequals(kids[i].name, "display")) return fail(error, "challenge without a display: " + kids[i].name);
    if (i + 1 >= kids.size()) return fail(error, "display without a challenge");
    Challenge c;
    c.title = kids[i].attr("title");
    c.description = kids[i].attr("description");
    Parser p{&c};
    c.root = p.node(kids[++i]);
    out.push_back(std::move(c));
  }
  return true;
}

bool Location::inside(double x, double z) const {
  for (size_t i = 0; i < points.size(); ++i) {
    const double dx = x - points[i][0], dz = z - points[i][2];
    if (dx * dx + dz * dz <= radius[i] * radius[i]) return true;
  }
  return false;
}

bool Location::inside_first(double x, double z) const {
  if (points.empty()) return false;
  const double dx = x - points[0][0], dz = z - points[0][2];
  return dx * dx + dz * dz <= radius[0] * radius[0];
}

bool parse_named_transforms(std::string_view xml, Locations& out, std::string* error) {
  audio::XmlNode root;
  if (!audio::parse_xml(xml, root, error)) return false;
  out.clear();
  for (const auto& t : root.children) {
    if (!audio::iequals(t.name, "NamedTransform")) continue;
    Location& l = out[lower(t.attr("name"))];
    for (const auto& p : t.children) {
      if (!audio::iequals(p.name, "Transform")) continue;
      l.points.push_back({p.number("pos.x"), p.number("pos.y"), p.number("pos.z")});
      l.radius.push_back(p.number("radius", kDefaultRadius));
    }
  }
  if (out.empty()) return fail(error, "no NamedTransform");
  return true;
}

size_t ClubData::challenges() const {
  size_t n = 0;
  for (const auto& [id, list] : cars) n += list.size();
  return n;
}

size_t ClubData::measured() const {
  size_t n = 0;
  for (const auto& [id, list] : cars)
    for (const auto& c : list) n += c.supported;
  return n;
}

bool load_club(const std::string& state_root, ClubData& out, std::string* error) {
  namespace fs = std::filesystem;
  out = {};
  if (state_root.empty()) return fail(error, "no state root");
  // user/<xuid>/4D5309C9/00000002/<package>/Media/DLCZips/<offer>/Media/carchallenges
  const fs::path content = fs::path(state_root) / "user" / "0000000000000000" / "4D5309C9" / "00000002";
  std::error_code ec;
  fs::path dir;
  for (const auto& pkg : fs::directory_iterator(content, ec)) {
    const fs::path zips = pkg.path() / "Media" / "DLCZips";
    for (const auto& offer : fs::directory_iterator(zips, ec)) {
      const fs::path d = offer.path() / "Media" / "carchallenges";
      if (fs::is_directory(d, ec)) dir = d, out.package = pkg.path().filename().string();
    }
  }
  if (dir.empty()) return fail(error, "the 1000 Club package is not installed");
  for (const auto& f : fs::directory_iterator(dir, ec)) {
    const std::string name = lower(f.path().filename().string());
    if (name.rfind("carchallenges_", 0) != 0 || f.path().extension() != ".xml") continue;
    const int64_t id = std::atoll(name.c_str() + 14);
    std::ifstream in(f.path(), std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string why;
    if (!parse_challenges(text, out.cars[id], &why)) return fail(error, name + ": " + why);
  }
  std::string why;
  auto zip = fh1::Archive::open(fs::path(state_root) / "title-update-v4" / "media.zip", &why);
  if (!zip) return fail(error, "title-update-v4/media.zip: " + why);
  auto nt = zip->read("carchallenges/named_transforms.xml");
  if (!nt.ok()) return fail(error, "named_transforms.xml: " + nt.error);
  const std::string_view text(reinterpret_cast<const char*>(nt.data.data()), nt.data.size());
  if (!parse_named_transforms(text, out.locations, &why)) return fail(error, "named_transforms.xml: " + why);
  return true;
}

Run::Run(const Challenge& challenge, const Locations& locations) : c_(&challenge), locs_(&locations) {
  std::string missing;
  check_locations(c_->root, *locs_, missing);
  unsupported_ = !c_->supported || !missing.empty();
  build(c_->root, root_);
}

void Run::build(const Node& n, State& st) {
  st.children.resize(n.children.size());
  for (size_t i = 0; i < n.children.size(); ++i) build(n.children[i], st.children[i]);
}

void Run::activate(const Node& n, State& st, double t, bool keep_saved) {
  st.active = true, st.done = false, st.start = t, st.current = 0, st.was_true = false, st.started = false;
  st.times.clear();
  if (!(keep_saved && n.save_progress)) st.count = 0;
  if (n.kind == Kind::kAToB) return;  // its children start with the trip
  for (size_t i = 0; i < n.children.size(); ++i) activate(n.children[i], st.children[i], t, keep_saved);
}

bool Run::holds(const Condition& c, const State& owner, const Signal& s) const {
  switch (c.kind) {
    case Cond::kSpeed: return compare(s.speed_mph, c.cmp, c.value);
    case Cond::kTimer: return compare(s.time - owner.start, c.cmp, c.value);
    case Cond::kGear: return compare(double(s.gear), c.cmp, c.value);
    case Cond::kDistance: return compare(s.jump_m, c.cmp, c.value);
    case Cond::kInAir: return s.in_air;
    case Cond::kCollided: return s.collided;
    case Cond::kLocation: {
      auto it = locs_->find(c.name);
      return it != locs_->end() && it->second.inside(s.x, s.z) == c.inside;
    }
    case Cond::kUnsupported: return false;
  }
  return false;
}

bool Run::progressed(const Node& n, const State& st) const {
  if (st.done || st.count > 0 || st.current > 0 || st.started) return true;
  for (size_t i = 0; i < n.children.size(); ++i)
    if (!is_fail(n.children[i]) && progressed(n.children[i], st.children[i])) return true;
  return false;
}

Run::Result Run::advance(const Node& n, State& st, const Signal& s) {
  if (st.done) return Result::kPassed;
  auto all = [&] {
    for (const auto& c : n.conditions)
      if (!holds(c, st, s)) return false;
    return !n.conditions.empty();
  };
  // A group's pass: once, or again until its pass_count.
  auto group_passed = [&] {
    if (++st.count >= n.pass_count) {
      st.done = true;
      return Result::kPassed;
    }
    for (size_t i = 0; i < n.children.size(); ++i) activate(n.children[i], st.children[i], s.time, false);
    st.current = 0;
    return Result::kRunning;
  };
  switch (n.kind) {
    case Kind::kFail: return all() ? Result::kFailed : Result::kRunning;
    case Kind::kFailUnless: return all() ? Result::kRunning : Result::kFailed;
    case Kind::kPass: {
      const bool h = all(), edge = h && !st.was_true;
      st.was_true = h;
      if (n.pass_count <= 1 && n.pass_interval <= 0) {
        if (h) st.done = true;
      } else if (edge) {
        if (n.pass_interval > 0) {
          st.times.push_back(s.time);
          while (!st.times.empty() && st.times.front() < s.time - n.pass_interval) st.times.pop_front();
          if (int(st.times.size()) >= n.pass_count) st.done = true;
        } else if (++st.count >= n.pass_count) {
          st.done = true;
        }
      }
      return st.done ? Result::kPassed : Result::kRunning;
    }
    case Kind::kAnd:
    case Kind::kOr: {
      bool every = true, any = false;
      for (size_t i = 0; i < n.children.size(); ++i) {
        const Result r = advance(n.children[i], st.children[i], s);
        if (r == Result::kFailed) return Result::kFailed;
        if (is_fail(n.children[i])) continue;
        every = every && r == Result::kPassed;
        any = any || r == Result::kPassed;
      }
      return (n.kind == Kind::kAnd ? every : any) ? group_passed() : Result::kRunning;
    }
    case Kind::kThen: {
      for (size_t i = 0; i < n.children.size(); ++i)
        if (is_fail(n.children[i]) && advance(n.children[i], st.children[i], s) == Result::kFailed)
          return Result::kFailed;
      // The sequence: the current step runs; when it passes the next one
      // starts (its timers from now).
      for (;;) {
        size_t step = 0, k = 0;
        for (; k < n.children.size(); ++k)
          if (!is_fail(n.children[k]) && int(step++) == st.current) break;
        if (k == n.children.size()) return group_passed();
        const Result r = advance(n.children[k], st.children[k], s);
        if (r == Result::kFailed) return Result::kFailed;
        if (r != Result::kPassed) return Result::kRunning;
        ++st.current;
        for (size_t j = k + 1; j < n.children.size(); ++j)
          if (!is_fail(n.children[j])) {
            activate(n.children[j], st.children[j], s.time, true);
            break;
          }
        // A step that passes at once lets the next one test this step too
        // (the loop); a fresh timer then reads 0.
      }
    }
    case Kind::kAToB: {
      const Location& a = locs_->at(n.start_loc);
      const Location& b = locs_->at(n.end_loc);
      if (!st.started) {
        if (!a.inside_first(s.x, s.z)) return Result::kRunning;
        st.started = true;
        st.start = s.time;
        for (size_t i = 0; i < n.children.size(); ++i) activate(n.children[i], st.children[i], s.time, true);
      }
      bool every = true;
      for (size_t i = 0; i < n.children.size(); ++i) {
        const Result r = advance(n.children[i], st.children[i], s);
        if (r == Result::kFailed) return Result::kFailed;
        if (!is_fail(n.children[i])) every = every && r == Result::kPassed;
      }
      if (every && b.inside(s.x, s.z)) {
        st.done = true;
        return Result::kPassed;
      }
      return Result::kRunning;
    }
  }
  return Result::kRunning;
}

bool Run::step(const Signal& s) {
  if (passed_ || unsupported_) return false;
  if (!root_.active) activate(c_->root, root_, s.time, true);
  const Result r = advance(c_->root, root_, s);
  if (r == Result::kPassed) return passed_ = true;
  if (r == Result::kFailed) {
    if (progressed(c_->root, root_)) ++failures_;
    activate(c_->root, root_, s.time, true);
  }
  return false;
}

bool Run::under_way(const Signal& s) const {
  if (passed_ || unsupported_ || !root_.active) return false;
  if (progressed(c_->root, root_)) return true;
  // A held condition (a group whose fail restarts it every step until it
  // holds); a trip waiting at A is not under way.
  std::vector<const Node*> stack{&c_->root};
  while (!stack.empty()) {
    const Node* n = stack.back();
    stack.pop_back();
    if (n->kind == Kind::kAToB) return false;
    for (const auto& k : n->children) stack.push_back(&k);
  }
  return s.time - root_.start > 1.0;
}

std::string Run::status(const Signal& s) const {
  if (passed_) return "done";
  if (unsupported_) {
    std::string missing = c_->unsupported;
    if (missing.empty()) check_locations(c_->root, *locs_, missing);
    return "not measured yet (" + missing + ")";
  }
  char b[160];
  // The trip, when there is one: waiting at A, or under way.
  std::vector<std::pair<const Node*, const State*>> stack{{&c_->root, &root_}};
  while (!stack.empty()) {
    auto [n, st] = stack.back();
    stack.pop_back();
    if (n->kind == Kind::kAToB) {
      if (!st->started) std::snprintf(b, sizeof b, "drive into %s", n->start_loc.c_str());
      else std::snprintf(b, sizeof b, "%.0f s on the way to %s", s.time - st->start, n->end_loc.c_str());
      return b;
    }
    for (size_t i = 0; i < n->children.size(); ++i) stack.push_back({&n->children[i], &st->children[i]});
  }
  if (c_->root.kind == Kind::kThen) {
    int steps = 0;
    for (const auto& k : c_->root.children) steps += !is_fail(k);
    std::snprintf(b, sizeof b, "step %d of %d, %.1f s", root_.current + 1, steps, s.time - root_.start);
  } else {
    std::snprintf(b, sizeof b, "%.1f s", s.time - root_.start);
  }
  return b;
}

}  // namespace freeroam::club
