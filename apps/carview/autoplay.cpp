#include "autoplay.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace carview {

namespace {
int64_t mono_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}  // namespace

bool Autoplay::load(const std::filesystem::path& script, const std::filesystem::path& out_dir, std::string* error) {
  std::ifstream in(script);
  if (!in) {
    if (error) *error = "cannot read " + script.string();
    return false;
  }
  std::string line;
  int n = 0;
  while (std::getline(in, line)) {
    ++n;
    if (const auto h = line.find('#'); h != std::string::npos) line.resize(h);
    std::istringstream row(line);
    Step s;
    if (!(row >> s.t)) continue;
    if (!(row >> s.cmd)) {
      if (error) *error = "line " + std::to_string(n) + ": no command";
      return false;
    }
    if (s.cmd == "every") {
      row >> s.num >> s.arg;
      if (s.num <= 0.0 || s.arg.empty()) {
        if (error) *error = "line " + std::to_string(n) + ": every <seconds> <prefix>";
        return false;
      }
    } else if (s.cmd == "key" || s.cmd == "capture" || s.cmd == "drive" || s.cmd == "mapto" || s.cmd == "launch") {
      if (!(row >> s.arg)) {
        if (error) *error = "line " + std::to_string(n) + ": " + s.cmd + " needs an argument";
        return false;
      }
    } else if (s.cmd != "stop") {
      if (error) *error = "line " + std::to_string(n) + ": unknown command " + s.cmd;
      return false;
    }
    steps_.push_back(s);
  }
  std::stable_sort(steps_.begin(), steps_.end(), [](const Step& a, const Step& b) { return a.t < b.t; });
  out_ = out_dir;
  std::error_code ec;
  std::filesystem::create_directories(out_, ec);
  if (FILE* f = std::fopen((out_ / "beats.csv").string().c_str(), "w")) {
    std::fputs("t,field,value\n", f);
    std::fclose(f);
  }
  t0_ns_ = mono_ns();
  active_ = true;
  return true;
}

double Autoplay::now() const { return active_ ? double(mono_ns() - t0_ns_) * 1e-9 : 0.0; }

void Autoplay::advance() {
  const double t = now();
  while (next_ < steps_.size() && steps_[next_].t <= t) {
    const Step& s = steps_[next_++];
    if (s.cmd == "key") keys_.push_back(s.arg);
    else if (s.cmd == "mapto") keys_.push_back("mapto:" + s.arg);
    else if (s.cmd == "launch") keys_.push_back("launch:" + s.arg);
    else if (s.cmd == "capture") captures_.push_back(s.arg);
    else if (s.cmd == "every") every_s_ = s.num, next_every_ = s.t, every_prefix_ = s.arg;
    else if (s.cmd == "drive") {
      drive_auto_ = s.arg == "auto";
      observe("autoplay_drive", s.arg);
    } else if (s.cmd == "stop") stop_ = true;
  }
  if (every_s_ > 0.0 && t >= next_every_) {
    char name[64];
    std::snprintf(name, sizeof name, "%s_%05d", every_prefix_.c_str(), int(next_every_ + 0.5));
    captures_.push_back(name);
    while (next_every_ <= t) next_every_ += every_s_;
  }
}

std::vector<std::string> Autoplay::take_due_keys() {
  if (!active_) return {};
  advance();
  std::vector<std::string> k;
  k.swap(keys_);
  for (const auto& key : k) observe("autoplay_key", key + "@" + std::to_string(now()));
  return k;
}

std::string Autoplay::take_due_capture() {
  if (!active_) return {};
  advance();
  if (captures_.empty()) return {};
  std::string c = captures_.front();
  captures_.erase(captures_.begin());
  return c;
}

void Autoplay::observe(const char* field, const std::string& value) {
  if (!active_) return;
  auto it = last_.find(field);
  if (it != last_.end() && it->second == value) return;
  last_[field] = value;
  if (FILE* f = std::fopen((out_ / "beats.csv").string().c_str(), "a")) {
    std::fprintf(f, "%.3f,%s,%s\n", now(), field, value.c_str());
    std::fclose(f);
  }
}

void Autoplay::note(const std::string& line) {
  if (!active_) return;
  if (FILE* f = std::fopen((out_ / "beats.csv").string().c_str(), "a")) {
    std::fprintf(f, "%.3f,note,%s\n", now(), line.c_str());
    std::fclose(f);
  }
}

}  // namespace carview
