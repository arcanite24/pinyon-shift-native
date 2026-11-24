#include "race/rally_codriver.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>

#include "audio/xml.h"
#include "fh1data/archive.h"

namespace race {
namespace {

std::string lower_str(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string humanize_sample(std::string_view id) {
  std::string out;
  for (size_t i = 0; i < id.size(); ++i) {
    const char c = id[i];
    if (c == '_') {
      out += ' ';
      continue;
    }
    if (i > 0 && std::isupper(static_cast<unsigned char>(c)) &&
        !std::isupper(static_cast<unsigned char>(id[i - 1])) && out.back() != ' ') {
      out += ' ';
    }
    out += char(std::toupper(static_cast<unsigned char>(c)));
  }
  return out;
}

bool is_modifier_sample(std::string_view id, std::string& chip_out) {
  const std::string l = lower_str(id);
  if (l == "30" || l == "40" || l == "50" || l == "60" || l == "70" || l == "80" || l == "100" || l == "150" ||
      l == "200" || l == "250" || l == "300") {
    chip_out = std::string(id) + "M";
    return true;
  }
  if (l.find("dontcut") != std::string::npos || l.find("don'tcut") != std::string::npos) {
    chip_out = "DON'T CUT";
    return true;
  }
  if (l == "cut") {
    chip_out = "CUT";
    return true;
  }
  if (l.find("narrow") != std::string::npos) {
    chip_out = "NARROWS";
    return true;
  }
  if (l.find("crest") != std::string::npos) {
    chip_out = "CREST";
    return true;
  }
  if (l.find("jump") != std::string::npos) {
    chip_out = "JUMP";
    return true;
  }
  if (l.find("caution") != std::string::npos) {
    chip_out = "CAUTION";
    return true;
  }
  if (l.find("bridge") != std::string::npos) {
    chip_out = "BRIDGE";
    return true;
  }
  if (l.find("tunnel") != std::string::npos) {
    chip_out = "TUNNEL";
    return true;
  }
  if (l.find("gravel") != std::string::npos || l.find("dirt") != std::string::npos) {
    chip_out = "GRAVEL";
    return true;
  }
  if (l.find("tarmac") != std::string::npos || l.find("asphalt") != std::string::npos) {
    chip_out = "TARMAC";
    return true;
  }
  if (l.find("keepmid") != std::string::npos) {
    chip_out = "KEEP MID";
    return true;
  }
  if (l.find("keepleft") != std::string::npos) {
    chip_out = "KEEP LEFT";
    return true;
  }
  if (l.find("keepright") != std::string::npos) {
    chip_out = "KEEP RIGHT";
    return true;
  }
  if (l.find("tightens") != std::string::npos) {
    chip_out = "TIGHTENS";
    return true;
  }
  return false;
}

}  // namespace

const CoDriverAudioEntry* CoDriverAudioMap::find(std::string_view play_id) const {
  auto it = by_id.find(lower_str(play_id));
  return it == by_id.end() ? nullptr : &it->second;
}

bool parse_codriver_audio_xml(std::string_view xml, CoDriverAudioMap& out, std::string* error) {
  out.by_id.clear();
  audio::XmlNode root;
  if (!audio::parse_xml(xml, root, error)) return false;
  for (const auto& c : root.children) {
    if (!audio::iequals(c.name, "event")) continue;
    CoDriverAudioEntry e;
    e.play_id = c.attr("play");
    e.group = c.attr("group");
    e.cue = c.attr("cue");
    e.icon = c.attr("icon", "None");
    if (e.play_id.empty()) continue;
    out.by_id[lower_str(e.play_id)] = std::move(e);
  }
  return !out.by_id.empty();
}

bool load_codriver_audio_map(const std::filesystem::path& rally_package_dir, CoDriverAudioMap& out,
                             std::string* error) {
  out.by_id.clear();
  if (rally_package_dir.empty()) {
    if (error) *error = "no Rally package directory";
    return false;
  }
  const auto p = rally_package_dir / "Media" / "DLCZips" / "1600_pri_65" / "Media" / "audio" / "VO" /
                 "CoDriverAudio.xml";
  std::ifstream in(p, std::ios::binary);
  if (!in) {
    if (error) *error = "cannot open " + p.string();
    return false;
  }
  std::string xml((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return parse_codriver_audio_xml(xml, out, error);
}

bool parse_rally_calls_xml(std::string_view xml, std::vector<RallyCallGate>& out, std::string* error) {
  out.clear();
  // Route XMLs can be large; parse cleanly with audio::parse_xml or extract RallyCalls block.
  const size_t rc_start = xml.find("<RallyCalls");
  if (rc_start == std::string_view::npos) return true;  // route without RallyCall gates (e.g. free roam)
  const size_t rc_end = xml.find("</RallyCalls>", rc_start);
  if (rc_end == std::string_view::npos) return true;
  const std::string_view block = xml.substr(rc_start, rc_end - rc_start + 13);

  audio::XmlNode root;
  if (!audio::parse_xml(block, root, error)) return false;

  for (const auto& node : root.children) {
    if (!audio::iequals(node.name, "RallyCall")) continue;
    RallyCallGate gate;
    gate.name = node.attr("name");
    gate.width = node.number("width", 20.0);
    if (gate.width <= 0.1) gate.width = 20.0;
    if (const auto* tr = node.child("Transform")) {
      gate.x = tr->number("pos.x");
      gate.y = tr->number("pos.y");
      gate.z = tr->number("pos.z");
      const double fx = tr->number("facing.x", 0.0);
      const double fz = tr->number("facing.z", 1.0);
      const double len = std::hypot(fx, fz);
      if (len > 1e-6) {
        gate.fx = fx / len;
        gate.fz = fz / len;
      }
    }
    if (const auto* samples = node.child("Samples")) {
      for (const auto& s : samples->children) {
        if (!audio::iequals(s.name, "Sample")) continue;
        std::string sid = s.attr("id");
        if (!sid.empty()) gate.sample_ids.push_back(std::move(sid));
      }
    }
    if (!gate.sample_ids.empty()) out.push_back(std::move(gate));
  }
  return true;
}

bool load_rally_calls(const std::filesystem::path& stage_world, const std::filesystem::path& rally_package_dir,
                      int route_id, std::vector<RallyCallGate>& out, std::string* error) {
  out.clear();
  char route_name[32];
  std::snprintf(route_name, sizeof route_name, "TrackRoute%03d.xml", route_id);

  // 1. Try loose route XML under stage_world/Ribbon_00/.
  if (!stage_world.empty()) {
    const auto p = stage_world / "Ribbon_00" / route_name;
    std::ifstream in(p, std::ios::binary);
    if (in) {
      std::string xml((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      return parse_rally_calls_xml(xml, out, error);
    }
  }

  // 2. Fallback: read directly from Rally DLC package's 1600_pri_65.zip.
  if (!rally_package_dir.empty()) {
    std::string why;
    auto zip = fh1::Archive::open(rally_package_dir / "Media" / "DLCZips" / "1600_pri_65.zip", &why);
    if (zip) {
      const std::string member = std::string("media/Tracks/ColoradoDirt/Ribbon_00/") + route_name;
      auto r = zip->read(member);
      if (r.ok()) {
        std::string_view sv(reinterpret_cast<const char*>(r.data.data()), r.data.size());
        return parse_rally_calls_xml(sv, out, error);
      }
    }
  }

  if (error) *error = std::string("Rally route XML not found for route ") + std::to_string(route_id);
  return false;
}

PaceNoteIcon classify_pace_icon(std::string_view icon_name, std::string_view sample_id) {
  PaceNoteIcon out;
  const std::string ic = lower_str(icon_name);
  const std::string sid = lower_str(sample_id);

  if (ic == "easyleft") {
    out.kind = PaceIconKind::kEasyLeft;
    out.severity = 5;
    out.dir = -1;
    out.label = "5L";
    return out;
  }
  if (ic == "easyright") {
    out.kind = PaceIconKind::kEasyRight;
    out.severity = 5;
    out.dir = +1;
    out.label = "5R";
    return out;
  }
  if (ic == "mediumleft") {
    out.kind = PaceIconKind::kMediumLeft;
    out.severity = 3;
    out.dir = -1;
    out.label = "3L";
    return out;
  }
  if (ic == "mediumright") {
    out.kind = PaceIconKind::kMediumRight;
    out.severity = 3;
    out.dir = +1;
    out.label = "3R";
    return out;
  }
  if (ic == "hardleft") {
    out.kind = PaceIconKind::kHardLeft;
    out.severity = 2;
    out.dir = -1;
    out.label = "2L";
    return out;
  }
  if (ic == "hardright") {
    out.kind = PaceIconKind::kHardRight;
    out.severity = 2;
    out.dir = +1;
    out.label = "2R";
    return out;
  }
  if (ic == "squareleft") {
    out.kind = PaceIconKind::kSquareLeft;
    out.severity = 1;
    out.dir = -1;
    out.label = "SQ L";
    return out;
  }
  if (ic == "squareright") {
    out.kind = PaceIconKind::kSquareRight;
    out.severity = 1;
    out.dir = +1;
    out.label = "SQ R";
    return out;
  }
  if (ic == "hairpinleft") {
    out.kind = PaceIconKind::kHairpinLeft;
    out.severity = 1;
    out.dir = -1;
    out.label = "HP L";
    return out;
  }
  if (ic == "hairpinright") {
    out.kind = PaceIconKind::kHairpinRight;
    out.severity = 1;
    out.dir = +1;
    out.label = "HP R";
    return out;
  }

  // Non-turn hazard icons inferred from sample_id when CoDriverAudio icon is "None".
  if (sid.find("jump") != std::string::npos) {
    out.kind = PaceIconKind::kJump;
    out.severity = 0;
    out.dir = 0;
    out.label = "JUMP";
    return out;
  }
  if (sid.find("crest") != std::string::npos) {
    out.kind = PaceIconKind::kCrest;
    out.severity = 0;
    out.dir = 0;
    out.label = "CREST";
    return out;
  }
  if (sid.find("caution") != std::string::npos) {
    out.kind = PaceIconKind::kCaution;
    out.severity = 0;
    out.dir = 0;
    out.label = "!";
    return out;
  }
  return out;
}

void RallyPaceNoteTracker::load(std::vector<RallyCallGate> gates, const CoDriverAudioMap& audio_map) {
  gates_ = std::move(gates);
  audio_ = audio_map;
  reset();
}

void RallyPaceNoteTracker::reset() {
  fired_at_.assign(gates_.size(), std::nullopt);
  has_prev_ = false;
  prev_x_ = prev_z_ = 0.0;
  last_time_ = 0.0;
  audio_queue_.clear();
  active_gate_ = -1;
  active_since_ = -1e9;
  active_sample_idx_ = 0;
}

size_t RallyPaceNoteTracker::fired_count() const {
  size_t n = 0;
  for (const auto& t : fired_at_)
    if (t.has_value()) ++n;
  return n;
}

void RallyPaceNoteTracker::rewind_to(double time_s) {
  audio_queue_.clear();
  for (auto& t : fired_at_)
    if (t && *t > time_s) t.reset();
  if (active_since_ > time_s) {
    active_gate_ = -1;
    active_since_ = -1e9;
    active_sample_idx_ = 0;
  }
  has_prev_ = false;
  last_time_ = time_s;
}

int RallyPaceNoteTracker::observe(double time_s, double x, double z, double speed_mps) {
  if (!std::isfinite(time_s) || !std::isfinite(x) || !std::isfinite(z)) return 0;

  // Rewind handling: un-fire any gate crossed after `time_s`.
  if (has_prev_ && time_s < last_time_ - 0.05) {
    rewind_to(time_s);
  }

  // Speed-scaled lookahead: call upcoming corners earlier at high speed so
  // pace notes finish before braking point (0 m <= 20 m/s, ramping up to 22 m).
  const double clamped_speed = std::isfinite(speed_mps) ? std::max(0.0, speed_mps) : 0.0;
  const double lookahead_m = std::clamp((clamped_speed - 20.0) * 0.45, 0.0, 22.0);

  int triggered = 0;
  if (has_prev_) {
    const double step_dist = std::hypot(x - prev_x_, z - prev_z_);
    if (step_dist > 1e-4 && step_dist <= 100.0) {
      std::vector<std::pair<double, size_t>> crossed;
      for (size_t i = 0; i < gates_.size(); ++i) {
        if (fired_at_[i].has_value()) continue;
        const auto& g = gates_[i];
        const double before = (prev_x_ - g.x) * g.fx + (prev_z_ - g.z) * g.fz + lookahead_m;
        const double after = (x - g.x) * g.fx + (z - g.z) * g.fz + lookahead_m;
        if (before >= 0.0 || after < 0.0) continue;
        const double frac = -before / (after - before);
        const double cx = prev_x_ + frac * (x - prev_x_) - g.x;
        const double cz = prev_z_ + frac * (z - prev_z_) - g.z;
        const double lateral = std::fabs(-cx * g.fz + cz * g.fx);
        if (lateral <= g.width * 0.5) crossed.emplace_back(frac, i);
      }
      std::sort(crossed.begin(), crossed.end());
      for (const auto& [frac, idx] : crossed) {
        const double cross_t = last_time_ + frac * std::max(0.0, time_s - last_time_);
        fired_at_[idx] = cross_t;
        // Drop stale unplayed cues from earlier gates when entering a newer call
        // so the co-driver never falls corners behind at race pace.
        if (active_gate_ != int(idx) && !audio_queue_.empty()) {
          audio_queue_.clear();
        }
        active_gate_ = int(idx);
        active_since_ = cross_t;
        active_sample_idx_ = 0;
        ++triggered;

        for (const auto& sid : gates_[idx].sample_ids) {
          CoDriverAudioCue q;
          q.sample_id = sid;
          if (const auto* e = audio_.find(sid)) {
            q.group = e->group;
            q.cue = e->cue;
          } else {
            q.group = "CoDriver/Turns";
            q.cue = sid;
          }
          audio_queue_.push_back(std::move(q));
        }
      }
    }
  }

  prev_x_ = x;
  prev_z_ = z;
  last_time_ = time_s;
  has_prev_ = true;
  return triggered;
}

bool RallyPaceNoteTracker::pop_audio_cue(CoDriverAudioCue& out) {
  if (audio_queue_.empty()) return false;
  out = std::move(audio_queue_.front());
  audio_queue_.pop_front();
  return true;
}

void RallyPaceNoteTracker::on_audio_cue_finished(double time_s) {
  ++active_sample_idx_;
  if (active_gate_ >= 0 && active_gate_ < int(gates_.size())) {
    if (active_sample_idx_ < gates_[size_t(active_gate_)].sample_ids.size()) active_since_ = time_s;
  }
}

PaceNoteHudState RallyPaceNoteTracker::hud(double time_s, double hold_seconds) const {
  PaceNoteHudState out;
  if (active_gate_ < 0 || active_gate_ >= int(gates_.size())) return out;
  const double age = time_s - active_since_;
  if (age < -0.1 || (audio_queue_.empty() && age > hold_seconds)) return out;

  const auto& g = gates_[size_t(active_gate_)];
  out.active = true;
  out.age_s = std::max(0.0, age);

  for (size_t i = 0; i < g.sample_ids.size(); ++i) {
    const std::string& sid = g.sample_ids[i];
    const auto* entry = audio_.find(sid);
    const std::string icon_name = entry ? entry->icon : "None";
    PaceNoteIcon icon = classify_pace_icon(icon_name, sid);
    if (icon.kind != PaceIconKind::kNone) {
      icon.active = (out.icons.empty() || i == active_sample_idx_);
      out.icons.push_back(std::move(icon));
    }
    std::string chip;
    if (is_modifier_sample(sid, chip)) {
      if (std::find(out.modifiers.begin(), out.modifiers.end(), chip) == out.modifiers.end())
        out.modifiers.push_back(std::move(chip));
    }
    if (!out.phrase.empty()) out.phrase += "  -  ";
    out.phrase += humanize_sample(sid);
  }
  return out;
}

}  // namespace race
