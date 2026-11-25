#include "cars/physics.h"

#include <charconv>
#include <cstdio>
#include <sstream>

#include "fh1data/archive.h"

namespace cars {
namespace {

bool number(std::string_view s, double& out) {
  if (s.empty()) return false;
  const char* end = s.data() + s.size();
  auto r = std::from_chars(s.data(), end, out);
  return r.ec == std::errc{} && r.ptr == end;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

std::string format(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.17g", v);
  return buf;
}

}  // namespace

bool parse_physics_settings(std::string_view text, PhysicsSettings& out, std::string* error) {
  out.values.clear();
  size_t lines = 0;
  while (!text.empty()) {
    const size_t nl = text.find('\n');
    std::string_view line = trim(text.substr(0, nl));
    text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
    ++lines;
    if (const size_t c = line.find(';'); c != std::string_view::npos) line = trim(line.substr(0, c));
    if (const size_t c = line.find("//"); c != std::string_view::npos) line = trim(line.substr(0, c));
    if (line.empty() || line.front() == '[') continue;
    const size_t space = line.find_first_of(" \t=");
    if (space == std::string_view::npos) continue;
    const std::string_view key = line.substr(0, space);
    std::string_view rest = trim(line.substr(space + 1));
    if (!rest.empty() && rest.front() == '=') rest = trim(rest.substr(1));
    double v = 0;
    // Flags are written true/false (GimbalLock\GimbalLockOn).
    if (rest == "true" || rest == "True" || rest == "TRUE") out.values[std::string(key)] = 1;
    else if (rest == "false" || rest == "False" || rest == "FALSE") out.values[std::string(key)] = 0;
    else if (number(rest, v)) out.values[std::string(key)] = v;
  }
  if (out.values.empty()) {
    if (error) *error = "PhysicsSettings.ini: no numeric settings in " + std::to_string(lines) + " lines";
    return false;
  }
  return true;
}

bool load_physics_settings(const std::filesystem::path& physics_zip, PhysicsSettings& out, std::string* error) {
  auto archive = fh1::Archive::open(physics_zip, error);
  if (!archive) return false;
  auto r = archive->read("physicssettings.ini");
  if (!r.ok()) {
    if (error) *error = physics_zip.string() + "!PhysicsSettings.ini: " + r.error;
    return false;
  }
  return parse_physics_settings(std::string_view(reinterpret_cast<const char*>(r.data.data()), r.data.size()), out,
                                error);
}

std::string physics_to_text(const CarPhysics& car, const PhysicsSettings& settings) {
  std::string out = "# shift-engine car physics rows (SPEC-CAR-PHYSICS), from the player's game database\n";
  out += "car " + std::to_string(car.car_id) + " " + car.media_name + "\n";
  for (const auto& [k, v] : car.values) out += "value " + k + " " + format(v) + "\n";
  for (const auto& [k, c] : car.curves) {
    out += "curve " + k + " " + std::to_string(c.size());
    for (double v : c) out += " " + format(v);
    out += "\n";
  }
  for (const auto& [k, v] : settings.values) out += "setting " + k + " " + format(v) + "\n";
  return out;
}

bool physics_from_text(std::string_view text, CarPhysics& car, PhysicsSettings& settings, std::string* error) {
  car = CarPhysics{};
  settings = PhysicsSettings{};
  std::istringstream in{std::string(text)};
  in.imbue(std::locale::classic());
  std::string line;
  int number_of_line = 0;
  auto fail = [&](const char* why) {
    if (error) *error = "physics text line " + std::to_string(number_of_line) + ": " + why;
    return false;
  };
  while (std::getline(in, line)) {
    ++number_of_line;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::istringstream l(line);
    l.imbue(std::locale::classic());
    std::string kind, key;
    l >> kind >> key;
    if (kind == "car") {
      car.car_id = std::stoll(key);
      std::getline(l >> std::ws, car.media_name);
    } else if (kind == "value" || kind == "setting") {
      double v = 0;
      if (!(l >> v)) return fail("no number");
      (kind == "value" ? car.values : settings.values)[key] = v;
    } else if (kind == "curve") {
      size_t n = 0;
      if (!(l >> n)) return fail("no curve length");
      std::vector<double> c(n);
      for (double& v : c)
        if (!(l >> v)) return fail("short curve");
      car.curves[key] = std::move(c);
    } else {
      return fail("unknown line");
    }
  }
  if (car.values.empty()) {
    if (error) *error = "physics text: no values";
    return false;
  }
  return true;
}

}  // namespace cars
