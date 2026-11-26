#include "audio/radio_dj.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "fh1data/archive.h"

namespace audio {
namespace {

std::string lower(std::string_view s) {
  std::string o(s);
  for (char& c : o) c = char(std::tolower(static_cast<unsigned char>(c)));
  return o;
}

bool truthy(const XmlNode& n, std::string_view attribute, bool fallback) {
  if (!n.has(attribute)) return fallback;
  const std::string v = lower(n.attr(attribute));
  return v == "true" || v == "1" || v == "yes";
}

bool fail(std::string* error, std::string why) {
  if (error) *error = std::move(why);
  return false;
}

void collect_rules(const XmlNode& state, std::vector<RadioDjWristbandRule>& into) {
  for (const auto& b : state.children) {
    if (!iequals(b.name, "Behaviour") || !iequals(b.attr("id"), "CRadioDJFestivalWristband")) continue;
    RadioDjWristbandRule r;
    r.event = b.attr("gameplayEvent");
    r.channel = truthy(b, "isFestivalUpdate", true) ? RadioChannel::kFestivalUpdate : RadioChannel::kSpecial;
    if (b.has("level")) r.level = int(std::lround(b.number("level")));
    r.after_events = std::max(0, int(std::lround(b.number("playAfterEventsNum"))));
    r.min_position = std::max(0, int(std::lround(b.number("playAfterEventsMinPos"))));
    if (!r.event.empty()) into.push_back(std::move(r));
  }
}

const XmlNode* find_state(const XmlNode& n, std::string_view id) {
  if (iequals(n.name, "State") && iequals(n.attr("id"), id)) return &n;
  for (const auto& c : n.children)
    if (const XmlNode* f = find_state(c, id)) return f;
  return nullptr;
}

}  // namespace

std::vector<RadioDjRequest> parse_radio_dj_requests(const XmlNode& requests) {
  std::vector<RadioDjRequest> out;
  for (const auto& c : requests.children) {
    RadioDjRequest r;
    if (iequals(c.name, "Special")) r.channel = RadioChannel::kSpecial;
    else if (iequals(c.name, "Immediate")) r.channel = RadioChannel::kImmediate;
    else continue;
    r.event = c.attr("gameplayEvent");
    if (r.event.empty()) continue;
    r.delay_min_s = c.has("timeDelayMin") ? c.number("timeDelayMin") : c.number("timeDelay");
    r.delay_max_s = std::max(r.delay_min_s, c.has("timeDelayMax") ? c.number("timeDelayMax") : r.delay_min_s);
    r.misc = int(std::lround(c.number("misc", -1)));
    r.enabled = truthy(c, "enabled", true);
    r.top_priority = truthy(c, "topPriority", false);
    out.push_back(std::move(r));
  }
  return out;
}

bool parse_radio_dj_rules(std::string_view radiodj_xml, RadioDjRules& out, std::string* error) {
  out = RadioDjRules{};
  XmlNode root;
  std::string why;
  if (!parse_xml(radiodj_xml, root, &why)) return fail(error, "radiodj.xml: " + why);
  if (const XmlNode* s = find_state(root, "festival_wristband")) collect_rules(*s, out.festival_wristband);
  if (const XmlNode* s = find_state(root, "special_wristband")) collect_rules(*s, out.special_wristband);
  if (out.festival_wristband.empty() && out.special_wristband.empty())
    return fail(error, "radiodj.xml: no CRadioDJFestivalWristband behaviours");
  return true;
}

bool load_radio_dj_rules(const std::filesystem::path& game_root, RadioDjRules& out, std::string* error) {
  std::string why;
  auto modes = fh1::Archive::open(game_root / "media" / "gamemodes.zip", &why);
  if (!modes) return fail(error, why);
  auto r = modes->read("radiodj.xml");
  if (!r.ok()) return fail(error, "gamemodes.zip radiodj.xml: " + r.error);
  return parse_radio_dj_rules(std::string_view(reinterpret_cast<const char*>(r.data.data()), r.data.size()), out,
                              error);
}

std::string wristband_rule_event(const RadioDjWristbandRule& rule, int level) {
  if (rule.cancels() || level < 0) return {};
  if (rule.level == RadioDjWristbandRule::kAnyLevel) return rule.event + std::to_string(level + 1);
  return rule.level == level ? rule.event : std::string{};
}

std::vector<RadioDjCue> RadioDjWristbands::on_awarded(int level, bool* cancel_pending) {
  std::vector<RadioDjCue> out;
  if (cancel_pending) *cancel_pending = false;
  if (!rules_ || level < 0 || !awarded_.insert(level).second) return out;
  for (size_t i = 0; i < rules_->festival_wristband.size(); ++i) {
    const auto& r = rules_->festival_wristband[i];
    if (r.cancels()) {
      if (cancel_pending) *cancel_pending = true;
      continue;
    }
    if (r.after_events > 0) continue;  // waits for the tier's results
    std::string ev = wristband_rule_event(r, level);  // each level awards once (awarded_)
    if (!ev.empty()) out.push_back({r.channel, std::move(ev)});
  }
  return out;
}

std::vector<RadioDjCue> RadioDjWristbands::on_ready(int level) {
  std::vector<RadioDjCue> out;
  if (!rules_ || level < 0 || !ready_.insert(level).second) return out;
  for (const auto& r : rules_->special_wristband) {
    std::string ev = wristband_rule_event(r, level);
    if (!ev.empty()) out.push_back({r.channel, std::move(ev)});
  }
  return out;
}

std::vector<RadioDjCue> RadioDjWristbands::on_event_finished(int event_level, int position) {
  std::vector<RadioDjCue> out;
  if (!rules_ || event_level < 0 || position <= 0) return out;
  counts_.resize(rules_->festival_wristband.size(), 0);
  for (size_t i = 0; i < rules_->festival_wristband.size(); ++i) {
    const auto& r = rules_->festival_wristband[i];
    if (r.after_events <= 0 || r.level != event_level || fired_.count(i)) continue;
    if (r.min_position > 0 && position > r.min_position) continue;
    if (++counts_[i] < r.after_events) continue;
    fired_.insert(i);
    out.push_back({r.channel, r.event});
  }
  return out;
}

std::vector<RadioDjCue> all_wristband_cues(const RadioDjRules& rules, int levels) {
  std::vector<RadioDjCue> out;
  auto add = [&](const std::vector<RadioDjWristbandRule>& list) {
    for (const auto& r : list)
      for (int l = 0; l < levels; ++l) {
        std::string ev = wristband_rule_event(r, l);
        if (ev.empty()) continue;
        RadioDjCue c{r.channel, std::move(ev)};
        if (std::find(out.begin(), out.end(), c) == out.end()) out.push_back(std::move(c));
      }
  };
  add(rules.festival_wristband);
  add(rules.special_wristband);
  return out;
}

bool radio_has_event(const RadioData& data, RadioChannel channel, std::string_view event) {
  const std::string key = lower(event);
  if (channel == RadioChannel::kFestivalUpdate) return data.festival_updates.count(key) != 0;
  for (const auto& st : data.stations) {
    const auto& m = channel == RadioChannel::kSpecial ? st.special_dialogue : st.immediate_dialogue;
    if (m.count(key)) return true;
  }
  return false;
}

RadioDjState parse_radio_dj_state(std::string_view name) {
  std::string key = lower(name);
  constexpr std::string_view kPrefix = "radiodj.";
  if (key.size() > kPrefix.size() && key.compare(0, kPrefix.size(), kPrefix) == 0) {
    key.erase(0, kPrefix.size());
  }
  if (key == "festival_wristband") return RadioDjState::kFestivalWristband;
  if (key == "special_wristband") return RadioDjState::kSpecialWristband;
  if (key == "post_race") return RadioDjState::kPostRace;
  if (key == "cycleradiologos") return RadioDjState::kCycleRadioLogos;
  return RadioDjState::kNone;
}

bool resolve_radio_event_channel(const RadioData& data, RadioChannel preferred, std::string_view event,
                                 RadioChannel& out_channel) {
  if (event.empty()) return false;
  if (radio_has_event(data, preferred, event)) {
    out_channel = preferred;
    return true;
  }
  for (RadioChannel ch : {RadioChannel::kImmediate, RadioChannel::kFestivalUpdate, RadioChannel::kSpecial}) {
    if (ch == preferred) continue;
    if (radio_has_event(data, ch, event)) {
      out_channel = ch;
      return true;
    }
  }
  return false;
}

}  // namespace audio
