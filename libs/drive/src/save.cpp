#include "drive/save.h"

#include <cmath>
#include <cstdio>
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
