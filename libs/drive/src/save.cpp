#include "drive/save.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <locale>
#include <sstream>
#include <system_error>

namespace drive {

namespace {

constexpr const char* kHeader = "shift-drive-save";

std::string num(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.6f", v);
  return buf;
}

bool fail(std::string* error, const std::string& what) {
  if (error) *error = what;
  return false;
}

}  // namespace

std::string save_to_text(const DriveSave& s) {
  std::string out = std::string(kHeader) + " " + std::to_string(DriveSave::kVersion) + "\n";
  out += "car " + s.car + "\n";
  out += "position " + num(s.position[0]) + " " + num(s.position[1]) + " " + num(s.position[2]) + "\n";
  out += "heading " + num(s.heading) + "\n";
  if (s.has_spawn)
    out += "spawn " + num(s.spawn[0]) + " " + num(s.spawn[1]) + " " + num(s.spawn[2]) + " " + num(s.spawn_heading) +
           "\n";
  if (s.total_distance > 0) out += "distance " + num(s.total_distance) + "\n";
  if (!s.signs_found.empty()) {
    out += "signs";
    for (const auto& id : s.signs_found) out += " " + id;
    out += "\n";
  }
  for (const auto& b : s.barns)
    out += "barn " + b.name + " " + std::to_string(b.status) + " " + num(b.rumour_at) + " " + num(b.found_at) + "\n";
  if (!s.garage.empty()) {
    out += "garage";
    for (int id : s.garage) out += " " + std::to_string(id);
    out += "\n";
  }
  if (s.owned_garage) out += "owned_garage 1\n";
  if (s.credits || s.total_winnings)
    out += "credits " + std::to_string(s.credits) + " " + std::to_string(s.total_winnings) + "\n";
  if (s.wristband_points) out += "wristband " + std::to_string(s.wristband_points) + "\n";
  if (s.num_races)
    out += "stats " + std::to_string(s.num_races) + " " + std::to_string(s.num_victories) + " " +
           std::to_string(s.num_podiums) + " " + num(s.time_festival) + " " + num(s.time_street) + " " +
           num(s.time_exhibition) + " " + num(s.top_speed) + "\n";
  for (const auto& [id, place] : s.race_results)
    out += "race_result " + std::to_string(id) + " " + std::to_string(place) + "\n";
  for (const auto& [id, place] : s.event_results)
    out += "event_result " + std::to_string(id) + " " + std::to_string(place) + "\n";
  if (!s.prize_cars.empty()) {
    out += "prize_cars";
    for (auto id : s.prize_cars) out += " " + std::to_string(id);
    out += "\n";
  }
  if (s.level) out += "level " + std::to_string(s.level) + "\n";
  for (const auto* list : {&s.event_unlocks, &s.hub_unlocks}) {
    if (list->empty()) continue;
    out += list == &s.event_unlocks ? "unlocks event" : "unlocks hub";
    for (auto id : *list) out += " " + std::to_string(id);
    out += "\n";
  }
  if (s.has_rally) out += "rally " + std::to_string(s.num_rallies) + " " + std::to_string(s.num_rally_stages) + "\n";
  for (const auto& car : s.club) {
    out += "club " + std::to_string(car.car_id);
    for (const auto& [key, done] : car.challenges) out += " " + std::to_string(key) + (done ? ":1" : ":0");
    out += "\n";
  }
  return out;
}

bool save_from_text(const std::string& text, DriveSave& out, std::string* error) {
  out = DriveSave{};
  std::istringstream in(text);
  in.imbue(std::locale::classic());
  std::string line;
  if (!std::getline(in, line)) return fail(error, "empty save");
  {
    std::istringstream h(line);
    h.imbue(std::locale::classic());
    std::string tag;
    int version = 0;
    if (!(h >> tag >> version) || tag != kHeader) return fail(error, "not a shift-drive-save file");
    if (version < 1 || version > DriveSave::kVersion)
      return fail(error, "save version " + std::to_string(version) + " (this build reads up to " +
                             std::to_string(DriveSave::kVersion) + ")");
  }
  bool have_pos = false, have_heading = false;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream l(line);
    l.imbue(std::locale::classic());
    std::string key;
    if (!(l >> key)) continue;
    if (key == "car") {
      l >> out.car;
    } else if (key == "position") {
      have_pos = bool(l >> out.position[0] >> out.position[1] >> out.position[2]);
    } else if (key == "heading") {
      have_heading = bool(l >> out.heading);
    } else if (key == "spawn") {
      out.has_spawn = bool(l >> out.spawn[0] >> out.spawn[1] >> out.spawn[2] >> out.spawn_heading);
    } else if (key == "distance") {
      if (!(l >> out.total_distance) || !std::isfinite(out.total_distance) || out.total_distance < 0)
        out.total_distance = 0;
    } else if (key == "signs") {
      for (std::string id; l >> id;) out.signs_found.push_back(id);
    } else if (key == "barn") {
      SavedBarn b;
      if (l >> b.name >> b.status >> b.rumour_at >> b.found_at) out.barns.push_back(b);
    } else if (key == "garage") {
      for (int id; l >> id;) out.garage.push_back(id);
    } else if (key == "owned_garage") {
      int v = 0;
      out.owned_garage = (l >> v) && v != 0;
    } else if (key == "credits") {
      l >> out.credits >> out.total_winnings;
    } else if (key == "wristband") {
      l >> out.wristband_points;
    } else if (key == "stats") {
      l >> out.num_races >> out.num_victories >> out.num_podiums >> out.time_festival >> out.time_street >>
          out.time_exhibition >> out.top_speed;
    } else if (key == "race_result" || key == "event_result") {
      int64_t id = 0;
      int place = 0;
      if (l >> id >> place && place >= 1)
        (key == "race_result" ? out.race_results : out.event_results).emplace_back(id, place);
    } else if (key == "prize_cars") {
      for (int64_t id; l >> id;) out.prize_cars.push_back(id);
    } else if (key == "level") {
      l >> out.level;
    } else if (key == "unlocks") {
      std::string kind;
      l >> kind;
      auto& list = kind == "event" ? out.event_unlocks : out.hub_unlocks;
      if (kind == "event" || kind == "hub")
        for (int64_t id; l >> id;) list.push_back(id);
    } else if (key == "rally") {
      out.has_rally = bool(l >> out.num_rallies >> out.num_rally_stages);
    } else if (key == "club") {
      SavedClubCar car;
      if (!(l >> car.car_id)) continue;
      for (std::string kv; l >> kv;) {
        const auto colon = kv.find(':');
        if (colon == std::string::npos) continue;
        car.challenges.emplace_back(uint32_t(std::strtoul(kv.c_str(), nullptr, 10)), kv.substr(colon + 1) == "1");
      }
      out.club.push_back(std::move(car));
    }
  }
  if (out.car.empty()) return fail(error, "save without a car");
  if (!have_pos || !have_heading) return fail(error, "save without a position and heading");
  for (double v : {out.position[0], out.position[1], out.position[2], out.heading})
    if (!std::isfinite(v)) return fail(error, "save with a non-finite position");
  return true;
}

bool read_save(const std::filesystem::path& path, DriveSave& out, std::string* error) {
  if (error) error->clear();
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return false;
  std::ifstream f(path, std::ios::binary);
  if (!f) return fail(error, "cannot read " + path.string());
  std::stringstream ss;
  ss << f.rdbuf();
  if (!save_from_text(ss.str(), out, error)) {
    if (error) *error = path.string() + ": " + *error;
    return false;
  }
  return true;
}

bool write_save(const std::filesystem::path& path, const DriveSave& save, std::string* error) {
  std::error_code ec;
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
  auto tmp = path;
  tmp += ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return fail(error, "cannot write " + tmp.string());
    f << save_to_text(save);
    if (!f.flush()) return fail(error, "cannot write " + tmp.string());
  }
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
    return fail(error, "cannot replace " + path.string());
  }
  return true;
}

}  // namespace drive
