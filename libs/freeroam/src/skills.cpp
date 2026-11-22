#include "freeroam/skills.h"

#include <algorithm>
#include <cmath>

#include "audio/xml.h"
#include "fh1data/archive.h"

namespace freeroam {
namespace {

constexpr double kMsToMph = 2.2369362920544;
constexpr double kRadToDeg = 57.29577951308232;

bool fail(std::string* error, const std::string& what) {
  if (error) *error = what;
  return false;
}

double first_grade_measure(const audio::XmlNode& ch, const char* primary) {
  if (primary && ch.has(primary)) return ch.number(primary, 0.0);
  for (const char* k : {"Time", "Distance", "SpeedMPH", "Speed", "SpeedDiffMPH", "SpeedDiff", "NumChainedSmashables",
                        "Objects", "Seconds", "SpeedReductionMPH", "Skills", "Threshold"}) {
    if (ch.has(k)) return ch.number(k, 0.0);
  }
  return 0.0;
}

void read_grades(const audio::XmlNode* node, const char* measure_attr, SkillConfig& out) {
  if (!node) return;
  out.id = node->name;
  out.retrigger_delay = node->number("RetriggerDelay", 0.0);
  out.object_type = node->attr("objectType");
  out.grades.clear();
  for (const auto& ch : node->children) {
    if (!audio::iequals(ch.name, "Grade")) continue;
    SkillGrade g;
    g.loc_string = ch.attr("LocString");
    g.threshold = first_grade_measure(ch, measure_attr);
    g.points = int(std::llround(ch.number("Fame", 100.0)));
    out.grades.push_back(std::move(g));
  }
}

const char* grade_prefix(int grade) {
  switch (grade) {
    case 4: return "ULTIMATE ";
    case 3: return "AWESOME ";
    case 2: return "GREAT ";
    default: return "";
  }
}

// Evaluates best grade (4..1) where measure >= threshold; returns {grade, points}.
std::pair<int, int> best_grade(const SkillConfig& cfg, double measure) {
  for (size_t i = 0; i < cfg.grades.size(); ++i) {
    if (measure >= cfg.grades[i].threshold) {
      const int grade = int(cfg.grades.size() - i);
      return {grade, cfg.grades[i].points};
    }
  }
  return {0, 0};
}

// Evaluates best grade (4..1) for time-lower-is-better skills (Turn180, JTurn).
std::pair<int, int> best_grade_lte(const SkillConfig& cfg, double elapsed) {
  for (size_t i = 0; i < cfg.grades.size(); ++i) {
    if (elapsed <= cfg.grades[i].threshold) {
      const int grade = int(cfg.grades.size() - i);
      return {grade, cfg.grades[i].points};
    }
  }
  return {0, 0};
}

const char* special_id_for(SmashObjectType t) {
  switch (t) {
    case SmashObjectType::kFence: return "Landscaping";
    case SmashObjectType::kMailbox: return "GoingPostal";
    case SmashObjectType::kRoadClosedSign: return "RoadOpen";
    case SmashObjectType::kTrashCan: return "Trashman";
    case SmashObjectType::kLogPiles: return "Lumberjack";
    case SmashObjectType::kFruitCart: return "FruitSalad";
    case SmashObjectType::kFireHydrant: return "Waterworks";
    default: return nullptr;
  }
}

const char* special_label_for(SmashObjectType t) {
  switch (t) {
    case SmashObjectType::kFence: return "LANDSCAPING";
    case SmashObjectType::kMailbox: return "GOING POSTAL";
    case SmashObjectType::kRoadClosedSign: return "ROAD OPEN";
    case SmashObjectType::kTrashCan: return "TRASHMAN";
    case SmashObjectType::kLogPiles: return "LUMBERJACK";
    case SmashObjectType::kFruitCart: return "FRUIT SALAD";
    case SmashObjectType::kFireHydrant: return "WATERWORKS";
    default: return nullptr;
  }
}

}  // namespace

const SkillConfig* SkillsData::find_skill(std::string_view id) const {
  for (const auto& s : all_skills)
    if (audio::iequals(s.id, id)) return &s;
  return nullptr;
}

int SkillsData::rank_for_points(double points) const {
  int rank = 250;
  for (const auto& lv : fame_levels)
    if (points >= lv.target) rank = std::min(rank, lv.rank);
  return rank;
}

double SkillsData::progress_in_rank(double points) const {
  if (fame_levels.empty()) return 0.0;
  double lo = 0.0;
  for (const auto& lv : fame_levels) {
    if (points < lv.target) {
      const double span = std::max(1.0, lv.target - lo);
      return std::clamp((points - lo) / span, 0.0, 1.0);
    }
    lo = lv.target;
  }
  return 1.0;
}

double SkillsData::next_rank_points(double points) const {
  for (const auto& lv : fame_levels)
    if (points < lv.target) return lv.target;
  return 0.0;
}

bool parse_skills_data(std::string_view fame_xml, std::string_view feats_xml, std::string_view sponsors_xml,
                       SkillsData& out, std::string* error) {
  out = SkillsData{};
  audio::XmlNode fame, feats, sponsors;
  std::string why;
  if (!audio::parse_xml(fame_xml, fame, &why)) return fail(error, "Fame.xml: " + why);
  if (!audio::parse_xml(feats_xml, feats, &why)) return fail(error, "HorizonFeats.xml: " + why);
  if (!audio::parse_xml(sponsors_xml, sponsors, &why)) return fail(error, "SponsorshipChallenges.xml: " + why);

  for (const auto& lv : fame.children) {
    if (!audio::iequals(lv.name, "Level")) continue;
    FameLevel fl;
    fl.rank = int(lv.number("rank", 250));
    fl.target = lv.number("target", 0);
    out.fame_levels.push_back(fl);
  }
  std::sort(out.fame_levels.begin(), out.fame_levels.end(),
            [](const FameLevel& a, const FameLevel& b) { return a.target < b.target; });

  out.world_collision_threshold = feats.number("WorldCollisionThreshold", 12.0);
  out.car_collision_threshold = feats.number("CarCollisionThreshold", 12.0);
  out.min_speed_to_cancel = feats.number("MinSpeedToCancelFeats", 10.0);
  out.delay_after_cancel = feats.number("DelayAfterCancelling", 3.0);
  out.tag_award_interval = feats.number("TagAwardInterval", 0.0);

  // Parse all 54 HorizonFeats children into the complete registry.
  for (const auto& ch : feats.children) {
    SkillConfig sc;
    read_grades(&ch, nullptr, sc);
    sc.param_d = ch.number("ComboWindowDuration", 0.0);
    out.all_skills.push_back(std::move(sc));
  }

  if (const auto* n = feats.child("Burnout")) {
    read_grades(n, "Time", out.burnout);
    out.burnout.param_a = n->number("WheelSpinThreshold", 10.0);
    out.burnout.param_b = n->number("MaxSpeed", 5.0);
  }
  if (const auto* n = feats.child("Donut")) read_grades(n, "Distance", out.donut);
  if (const auto* n = feats.child("Turn180")) {
    read_grades(n, "Time", out.turn180);
    out.turn180.param_a = n->number("MinSpeedToStart", 30.0);
    out.turn180.param_b = n->number("MinSpeedToEnd", 22.0);
    out.turn180.param_c = n->number("MinAngularChange", 140.0);
    out.turn180.param_d = n->number("MinSpeedDuring180", 8.0);
  }
  if (const auto* n = feats.child("JTurn")) {
    read_grades(n, "Time", out.jturn);
    out.jturn.param_a = n->number("MinSpeedToStart", 20.0);
    out.jturn.param_b = n->number("MinSpeedToEnd", 20.0);
    out.jturn.param_c = n->number("MinAngularChange", 120.0);
  }
  if (const auto* n = feats.child("Air")) read_grades(n, "Time", out.air);
  if (const auto* n = feats.child("Drift")) {
    read_grades(n, "Distance", out.drift);
    out.drift.param_a = n->number("MinSpeed", 25.0);
    out.drift.param_b = n->number("MinAngle", 15.0);
    out.drift.param_c = n->number("MaxAngle", 110.0);
  }
  if (const auto* n = feats.child("EDrift")) {
    read_grades(n, "Distance", out.edrift);
    out.edrift.param_a = n->number("MinSpeed", 25.0);
    out.edrift.param_b = n->number("MinAngle", 15.0);
    out.edrift.param_c = n->number("MaxAngle", 110.0);
    out.edrift.param_d = n->number("MinHandbrakeTime", 1.0);
  }
  if (const auto* n = feats.child("Draft")) {
    read_grades(n, "Time", out.draft);
    out.draft.param_a = n->has("MinSpeedThreshold") ? n->number("MinSpeedThreshold", 50.0) : n->number("MinSpeed", 50.0);
    out.draft.param_b = n->has("MinDraftThreshold") ? n->number("MinDraftThreshold", 0.1) : n->number("MinDraftFactor", 0.1);
  }
  if (const auto* n = feats.child("TwoWheels")) {
    read_grades(n, "Time", out.two_wheels);
    out.two_wheels.param_a = n->number("MinSpeed", 45.0);
  }
  if (const auto* n = feats.child("CleanSpeed")) {
    read_grades(n, "SpeedMPH", out.clean_speed);
    out.clean_speed.param_a = n->number("SpeedThreshold", 100.0);
    out.clean_speed.param_b = n->has("MetersPerAward") ? n->number("MetersPerAward", 450.0) : n->number("DistancePerAward", 450.0);
  }
  if (const auto* n = feats.child("NearMiss")) {
    read_grades(n, "SpeedMPH", out.near_miss);
    out.near_miss.param_a = n->has("MinPlayerSpeedMPH") ? n->number("MinPlayerSpeedMPH", 50.0) : n->number("MinSpeed", 50.0);
    out.near_miss.param_b = n->number("MaxDistance", 4.0);
  }
  if (const auto* n = feats.child("SketchyOvertake")) {
    read_grades(n, "SpeedDiffMPH", out.sketchy_overtake);
    out.sketchy_overtake.param_a = n->number("PenaltyAgeThreshold", 4.0);
  }
  if (const auto* n = feats.child("TradingPaint")) {
    read_grades(n, "SpeedMPH", out.trading_paint);
    out.trading_paint.param_a = n->number("MinPlayerSpeedMPH", 50.0);
    out.trading_paint.param_b = n->number("MaxDistance", 3.2);
  }
  if (const auto* n = feats.child("Pass")) {
    read_grades(n, "SpeedDiffMPH", out.pass);
    out.pass.param_a = n->number("PassResetTime", 4.0);
  }
  if (const auto* n = feats.child("Destruction")) {
    read_grades(n, "NumChainedSmashables", out.destruction);
    out.destruction.param_a = n->has("MaxTimeBetweenCollisions") ? n->number("MaxTimeBetweenCollisions", 4.0)
                                                                 : n->number("ChainWindow", 4.0);
  }

  // Parse all SponsorshipChallenges.xml categories (Fame + all 13 SkillChallenge categories).
  for (const auto& ch : sponsors.children) {
    if (!audio::iequals(ch.name, "Challenge")) continue;
    SponsorChallenge sc;
    sc.id = ch.attr("id");
    sc.type = ch.attr("type");
    sc.sponsor_name = ch.attr("sponsorName");
    sc.challenge_name = ch.attr("challengeName");
    if (audio::iequals(sc.type, "FameRankChallenge")) {
      for (const auto& rk : ch.children) {
        if (!audio::iequals(rk.name, "Rank")) continue;
        FameSponsorTier t;
        t.rank_required = int(rk.number("levelRequired", 225));
        t.credits = int64_t(std::llround(rk.number("credits", 0)));
        if (t.credits > 0) out.fame_tiers.push_back(t);
        SkillSponsorTier st;
        st.skill = "Fame";
        st.number = t.rank_required;
        st.credits = t.credits;
        sc.tiers.push_back(st);
      }
    } else if (audio::iequals(sc.type, "SkillChallenge")) {
      for (const auto& rk : ch.children) {
        if (!audio::iequals(rk.name, "Rank")) continue;
        SkillSponsorTier st;
        st.skill = rk.attr("skill");
        st.grade = int(rk.number("grade", 4));
        st.number = int(rk.number("number", 25));
        st.credits = int64_t(std::llround(rk.number("credits", 0)));
        if (st.credits > 0) sc.tiers.push_back(st);
      }
      std::sort(sc.tiers.begin(), sc.tiers.end(),
                [](const SkillSponsorTier& a, const SkillSponsorTier& b) { return a.number < b.number; });
    }
    if (!sc.id.empty()) out.sponsor_challenges.push_back(std::move(sc));
  }
  std::sort(out.fame_tiers.begin(), out.fame_tiers.end(),
            [](const FameSponsorTier& a, const FameSponsorTier& b) { return a.rank_required > b.rank_required; });

  return !out.fame_levels.empty();
}

bool load_skills_data(const std::filesystem::path& game_root, SkillsData& out, std::string* error) {
  std::string why;
  auto zip = fh1::Archive::open(game_root / "media" / "gametunablesettings.zip", &why);
  if (!zip) return fail(error, why);
  auto fame = zip->read("Fame.xml");
  if (!fame.ok()) return fail(error, "Fame.xml: " + fame.error);
  auto feats = zip->read("HorizonFeats.xml");
  if (!feats.ok()) return fail(error, "HorizonFeats.xml: " + feats.error);
  auto sponsors = zip->read("SponsorshipChallenges.xml");
  if (!sponsors.ok()) return fail(error, "SponsorshipChallenges.xml: " + sponsors.error);
  auto sv = [](const std::vector<uint8_t>& d) { return std::string_view(reinterpret_cast<const char*>(d.data()), d.size()); };
  return parse_skills_data(sv(fame.data), sv(feats.data), sv(sponsors.data), out, error);
}

SkillTracker::SkillTracker(const SkillsData& data, double initial_popularity, int initial_fame_claimed,
                           int64_t best_score, int64_t best_car)
    : data_(data),
      popularity_(initial_popularity),
      fame_claimed_(initial_fame_claimed),
      max_score_(best_score),
      max_car_(best_car) {}

void SkillTracker::set_sponsor_progress(const std::string& id, int count, int claimed) {
  sponsors_[id] = {std::max(0, count), std::max(0, claimed)};
}

void SkillTracker::clear_active_chain() {
  chain_points_ = 0;
  skill_count_ = 0;
  over9000_paid_ = false;
  pending_tags_.clear();
  burnout_t_ = donut_yaw_ = donut_dist_ = air_t_ = two_wheels_t_ = 0.0;
  turn180_active_ = jturn_active_ = false;
  turn180_t_ = turn180_yaw_ = jturn_t_ = jturn_yaw_ = 0.0;
  drift_dist_ = drift_hb_t_ = draft_t_ = speed_dist_ = speed_peak_mph_ = 0.0;
  drift_is_e_ = false;
  destruct_count_ = 0;
  recent_.clear();
}

void SkillTracker::reset_session_clock() {
  clear_active_chain();
  last_award_at_ = last_tag_at_ = lockout_until_ = -1e9;
  banked_at_ = lost_at_ = rank_up_at_ = -1e9;
  burnout_cd_ = donut_cd_ = turn180_cd_ = jturn_cd_ = air_cd_ = two_wheels_cd_ = -1e9;
  drift_cd_ = draft_cd_ = near_miss_cd_ = paint_cd_ = pass_cd_ = destruct_last_t_ = last_collision_t_ = -1e9;
  in_near_miss_ = false;
  combo_cd_.clear();
  special_cd_.clear();
  last_skill_.clear();
  last_points_ = 0;
  banked_total_ = 0;
}

void SkillTracker::record_sponsor_skill(const std::string& canonical_id, int grade, bool is_combo,
                                        std::vector<SkillEvent>& out) {
  // SponsorshipChallenges.xml grade convention: 0 = Ultimate (grade 4), 1 = Awesome (3),
  // 2 = Great (2), 3/4 = Any (grade >= 1).
  const int xml_grade = is_combo ? 0 : std::clamp(4 - grade, 0, 3);
  for (const auto& sc : data_.sponsor_challenges) {
    if (!audio::iequals(sc.type, "SkillChallenge") || sc.tiers.empty()) continue;
    const std::string& req_skill = sc.tiers.front().skill;
    bool matches = false;
    if (audio::iequals(req_skill, "Any")) matches = true;
    else if (audio::iequals(req_skill, "AnySkill")) matches = !is_combo;
    else if (audio::iequals(req_skill, "AnyCombo")) matches = is_combo;
    else if (audio::iequals(req_skill, canonical_id)) matches = true;
    else if (audio::iequals(req_skill, "SideSwipe") && audio::iequals(canonical_id, "ESideSwipe")) matches = true;
    if (!matches) continue;
    if (xml_grade > sc.tiers.front().grade) continue;
    auto& prog = sponsors_[sc.id];
    ++prog.count;
    while (size_t(prog.claimed_tiers) < sc.tiers.size() &&
           prog.count >= sc.tiers[size_t(prog.claimed_tiers)].number) {
      const auto& tier = sc.tiers[size_t(prog.claimed_tiers)];
      ++prog.claimed_tiers;
      out.push_back({SkillEvent::kSponsorChallenge,
                     "SPONSOR TIER " + std::to_string(prog.claimed_tiers) + ": " + sc.id, sc.id, 0, 1,
                     prog.claimed_tiers, tier.credits});
    }
  }
}

void SkillTracker::emit_tag(const PendingTag& tag, double time, std::vector<SkillEvent>& out) {
  if (tag.points <= 0 || time < lockout_until_) return;
  chain_points_ += tag.points;
  ++skill_count_;
  last_award_at_ = time;
  last_tag_at_ = time;
  last_skill_ = tag.label;
  last_points_ = tag.points;
  const int mult = skill_count_ >= 6 ? 3 : skill_count_ >= 3 ? 2 : 1;
  out.push_back(
      {SkillEvent::kSkillAwarded, tag.label, tag.canonical_id, tag.points, mult, popularity_rank(), 0, tag.grade});
  record_sponsor_skill(tag.canonical_id, tag.grade, tag.is_combo, out);
  if (tag.id != kCombo) {
    recent_.push_back({tag.id, tag.canonical_id, tag.grade, time});
    recent_.erase(std::remove_if(recent_.begin(), recent_.end(), [&](const RecentSkill& r) { return time - r.time > 36.0; }),
                  recent_.end());
    check_combos(time, out);
  }
  if (!over9000_paid_ && chain_points_ >= 9000) {
    over9000_paid_ = true;
    queue_award(kCombo, "Over9000", "OVER 9000!", 1000, 4, true, time, out);
  }
}

void SkillTracker::queue_award(SkillId id, std::string canonical_id, std::string label, int points, int grade,
                               bool is_combo, double time, std::vector<SkillEvent>& out) {
  if (points <= 0 || time < lockout_until_) return;
  PendingTag tag{id, std::move(canonical_id), std::move(label), points, grade, is_combo};
  // Enforce TagAwardInterval (0.3 s): emit immediately if clear, else queue cleanly.
  if (pending_tags_.empty() && time - last_tag_at_ >= data_.tag_award_interval) {
    emit_tag(tag, time, out);
  } else {
    pending_tags_.push_back(std::move(tag));
  }
}

void SkillTracker::flush_tags(double time, std::vector<SkillEvent>& out) {
  while (!pending_tags_.empty() && time >= lockout_until_ && time - last_tag_at_ >= data_.tag_award_interval) {
    PendingTag next = std::move(pending_tags_.front());
    pending_tags_.pop_front();
    emit_tag(next, time, out);
  }
}

std::vector<SkillEvent> SkillTracker::award_named(std::string_view skill_id, int grade, double time) {
  std::vector<SkillEvent> out;
  const SkillConfig* sc = data_.find_skill(skill_id);
  int pts = 500;
  bool is_combo = false;
  if (sc) {
    is_combo = sc->param_d > 0.0;
    if (!sc->grades.empty()) {
      const size_t idx = size_t(std::clamp(int(sc->grades.size()) - std::max(1, grade), 0, int(sc->grades.size()) - 1));
      pts = sc->grades[idx].points;
    }
  }
  std::string upper(skill_id);
  for (char& c : upper)
    if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
  emit_tag({kSpecial, std::string(skill_id), upper, pts, std::max(1, grade), is_combo}, time, out);
  return out;
}

void SkillTracker::check_combos(double time, std::vector<SkillEvent>& out) {
  auto ready = [&](const std::string& name, double cd) {
    for (auto& [k, t] : combo_cd_)
      if (k == name) {
        if (time - t < cd) return false;
        t = time;
        return true;
      }
    combo_cd_.push_back({name, time});
    return true;
  };
  auto count_recent = [&](SkillId id, double window) {
    int n = 0;
    for (const auto& r : recent_)
      if (r.id == id && time - r.time <= window) ++n;
    return n;
  };
  auto has_pair = [&](SkillId a, SkillId b, double window, bool ordered, int min_a_grade = 1) {
    double ta = -1e9, tb = -1e9;
    for (const auto& r : recent_) {
      if (r.id == a && r.grade >= min_a_grade && time - r.time <= window) ta = std::max(ta, r.time);
      if (r.id == b && time - r.time <= window) tb = std::max(tb, r.time);
    }
    if (ta < 0 || tb < 0) return false;
    return ordered ? (ta <= tb && tb - ta <= window) : (std::fabs(tb - ta) <= window);
  };

  if (count_recent(kNearMiss, 10.0) >= 3 && ready("DareDevil", 10.0))
    queue_award(kCombo, "Daredevil", "DAREDEVIL", 1000, 4, true, time, out);
  if (count_recent(kAir, 5.0) >= 3 && ready("Kangaroo", 5.0))
    queue_award(kCombo, "Kangaroo", "KANGAROO", 1000, 4, true, time, out);
  if (count_recent(kPass, 2.0) >= 3 && ready("TriplePass", 2.0))
    queue_award(kCombo, "TriplePass", "TRIPLE PASS", 1000, 4, true, time, out);
  if (count_recent(kDestruction, 15.0) >= 8 && ready("WreckingBall", 15.0))
    queue_award(kCombo, "WreckingBall", "WRECKING BALL", 1000, 4, true, time, out);
  if (count_recent(kNearMiss, 0.8) >= 2 && ready("Weaver", 1.5))
    queue_award(kCombo, "Weaver", "WEAVER", 1000, 4, true, time, out);
  if (has_pair(kAir, kNearMiss, 2.5, false) && ready("Stuntman", 3.0))
    queue_award(kCombo, "Stuntman", "STUNTMAN", 1000, 4, true, time, out);
  if (has_pair(kAir, kPass, 2.5, false) && ready("Superman", 3.0))
    queue_award(kCombo, "Superman", "SUPERMAN", 1000, 4, true, time, out);
  if (has_pair(kAir, kDestruction, 1.5, false) && ready("CrashLanding", 2.0))
    queue_award(kCombo, "CrashLanding", "CRASH LANDING", 1000, 4, true, time, out);
  if (has_pair(kBurnout, kCleanSpeed, 35.0, true) && ready("QuickOffTheMark", 35.0))
    queue_award(kCombo, "QuickOffTheMark", "QUICK OFF THE MARK", 1000, 4, true, time, out);
  if (has_pair(kDraft, kPass, 5.0, true) && ready("SlingShot", 5.0))
    queue_award(kCombo, "SlingShot", "SLINGSHOT", 1000, 4, true, time, out);
  if (has_pair(kDrift, kDestruction, 3.0, true) && ready("SideSwipe", 3.0))
    queue_award(kCombo, "SideSwipe", "SIDESWIPE", 1000, 4, true, time, out);
  if (has_pair(kEDrift, kDestruction, 3.0, true) && ready("ESideSwipe", 3.0))
    queue_award(kCombo, "ESideSwipe", "E-SIDESWIPE", 1000, 4, true, time, out);
  if (has_pair(kDrift, kPass, 2.5, true) && ready("ShowOff", 3.0))
    queue_award(kCombo, "ShowOff", "SHOW OFF", 1000, 4, true, time, out);
  if (has_pair(kEDrift, kPass, 2.5, true) && ready("EShowOff", 3.0))
    queue_award(kCombo, "EShowOff", "E-SHOW OFF", 1000, 4, true, time, out);
  if (has_pair(kDrift, kAir, 0.5, false, 2) && ready("EbisuStyle", 3.0))
    queue_award(kCombo, "EbisuStyle", "EBISU STYLE", 1000, 4, true, time, out);
  if (has_pair(kEDrift, kAir, 0.5, false, 2) && ready("EEbisuStyle", 3.0))
    queue_award(kCombo, "EEbisuStyle", "E-EBISU STYLE", 1000, 4, true, time, out);
  if (has_pair(kDrift, kNearMiss, 2.5, true) && ready("LuckyEscape", 3.0))
    queue_award(kCombo, "LuckyEscape", "LUCKY ESCAPE", 1000, 4, true, time, out);
  if (has_pair(kEDrift, kNearMiss, 2.5, true) && ready("ELuckyEscape", 3.0))
    queue_award(kCombo, "ELuckyEscape", "E-LUCKY ESCAPE", 1000, 4, true, time, out);
}

void SkillTracker::bank_chain(double time, std::vector<SkillEvent>& out) {
  // Drain any queued tags into the chain before banking.
  while (!pending_tags_.empty()) {
    PendingTag next = std::move(pending_tags_.front());
    pending_tags_.pop_front();
    emit_tag(next, time, out);
  }
  if (chain_points_ <= 0) return;
  const int mult = skill_count_ >= 6 ? 3 : skill_count_ >= 3 ? 2 : 1;
  const int total = chain_points_ * mult;
  const int rank_before = popularity_rank();
  popularity_ += double(total);
  race_banked_ += total;
  const int rank_after = popularity_rank();
  if (total > max_score_) {
    max_score_ = total;
    if (current_car_ > 0) max_car_ = current_car_;
  }
  banked_total_ = total;
  banked_at_ = time;
  out.push_back({SkillEvent::kChainBanked, "SKILL CHAIN BANKED", "Bank", total, mult, rank_after, 0});
  if (rank_after < rank_before) {
    rank_up_at_ = time;
    out.push_back({SkillEvent::kRankUp, "POPULARITY RANK UP", "Fame", 0, 1, rank_after, 0});
    for (size_t i = size_t(std::max(0, fame_claimed_)); i < data_.fame_tiers.size(); ++i) {
      if (rank_after <= data_.fame_tiers[i].rank_required) {
        fame_claimed_ = int(i + 1);
        out.push_back({SkillEvent::kFameSponsor, "FAME SPONSOR BONUS", "Fame", 0, 1,
                       data_.fame_tiers[i].rank_required, data_.fame_tiers[i].credits});
      }
    }
  }
  clear_active_chain();
}

void SkillTracker::cancel_chain(double time, std::vector<SkillEvent>& out) {
  if (chain_points_ > 0) {
    lost_at_ = time;
    out.push_back({SkillEvent::kChainLost, "SKILL CHAIN LOST", "Cancel", chain_points_, 1, popularity_rank(), 0});
  }
  clear_active_chain();
  lockout_until_ = time + data_.delay_after_cancel;
}

std::vector<SkillEvent> SkillTracker::step(const SkillInput& in) {
  std::vector<SkillEvent> out;
  if (in.dt <= 0) return out;
  if (in.car_id > 0) current_car_ = in.car_id;
  const double mph = std::fabs(in.speed_ms) * kMsToMph;
  const double fwd_mph = in.forward_speed_ms * kMsToMph;

  // Drain any pending tags respecting TagAwardInterval (0.3 s).
  flush_tags(in.time, out);

  // Cancel on hard collision (World/CarCollisionThreshold 12 m/s).
  if (in.world_impact_ms >= data_.world_collision_threshold || in.car_impact_ms >= data_.car_collision_threshold) {
    last_collision_t_ = in.time;
    if (chain_points_ > 0 || burnout_t_ > 0 || drift_dist_ > 0 || air_t_ > 0) cancel_chain(in.time, out);
    return out;
  }
  if (in.car_impact_ms > 0.3 || in.world_impact_ms > 0.5) last_collision_t_ = in.time;

  // MinSpeedToCancelFeats (10 mph): cancel active chain when slowing below threshold
  // (unless actively performing a Burnout or Donut), vs bank on normal timeout at speed.
  if (chain_points_ > 0) {
    const bool stationary_feat = burnout_t_ > 0.1 || donut_yaw_ > 0.5;
    if (mph < data_.min_speed_to_cancel && !stationary_feat && in.time - last_award_at_ > 1.0) {
      cancel_chain(in.time, out);
      return out;
    } else if (in.time - last_award_at_ >= data_.bank_timeout && pending_tags_.empty() && drift_dist_ <= 1.0 &&
               air_t_ <= 0.05 && burnout_t_ <= 0.1 && draft_t_ <= 0.1 && two_wheels_t_ <= 0.05) {
      bank_chain(in.time, out);
    }
  }

  if (in.time < lockout_until_) return out;

  // 1. Burnout & 2. Donut
  {
    const double spin_thr = data_.burnout.param_a > 0 ? data_.burnout.param_a : 10.0;
    const double max_burn_mph = std::max(5.0, data_.burnout.param_b * kMsToMph);
    const bool spinning = in.wheels_on_ground >= 2 && in.wheel_spin >= spin_thr && mph <= max_burn_mph;
    const bool turning_spin = in.wheels_on_ground >= 2 && in.wheel_spin >= spin_thr * 0.7 && mph <= 25.0 &&
                              std::fabs(in.yaw_rate) >= 1.2;
    if (turning_spin) {
      donut_yaw_ += std::fabs(in.yaw_rate) * in.dt;
      donut_dist_ += std::fabs(in.speed_ms) * in.dt;
    } else if (donut_yaw_ > 0.0) {
      if (donut_yaw_ >= 5.2 && in.time >= donut_cd_) {  // ~300+ deg spin
        const int g = donut_yaw_ >= 18.0 ? 4 : donut_yaw_ >= 12.0 ? 3 : donut_yaw_ >= 8.0 ? 2 : 1;
        const int pts = g == 4 ? 500 : g == 3 ? 250 : g == 2 ? 150 : 100;
        queue_award(kDonut, "Donut", std::string(grade_prefix(g)) + "DONUT", pts, g, false, in.time, out);
        donut_cd_ = in.time + std::max(1.0, data_.donut.retrigger_delay);
      }
      donut_yaw_ = donut_dist_ = 0.0;
    }
    if (spinning && !turning_spin) {
      burnout_t_ += in.dt;
    } else if (burnout_t_ > 0) {
      if (in.time >= burnout_cd_) {
        const auto [g, pts] = best_grade(data_.burnout, burnout_t_);
        if (g > 0) {
          queue_award(kBurnout, "Burnout", std::string(grade_prefix(g)) + "BURNOUT", pts, g, false, in.time, out);
          burnout_cd_ = in.time + data_.burnout.retrigger_delay;
        }
      }
      burnout_t_ = 0.0;
    }
  }

  // 3. Turn180 (forward > 30 mph -> 140+ deg rotation -> exit > 22 mph) & 4. JTurn (reverse > 20 mph -> 120+ deg -> forward > 20 mph)
  {
    const double t180_start = data_.turn180.param_a > 0 ? data_.turn180.param_a : 30.0;
    const double t180_end = data_.turn180.param_b > 0 ? data_.turn180.param_b : 22.0;
    const double t180_ang = (data_.turn180.param_c > 0 ? data_.turn180.param_c : 140.0) / kRadToDeg;
    const double t180_min = data_.turn180.param_d > 0 ? data_.turn180.param_d : 8.0;
    if (!turn180_active_) {
      if (fwd_mph >= t180_start && std::fabs(in.yaw_rate) >= 1.5 && in.time >= turn180_cd_) {
        turn180_active_ = true;
        turn180_t_ = 0.0;
        turn180_yaw_ = 0.0;
      }
    } else {
      turn180_t_ += in.dt;
      turn180_yaw_ += std::fabs(in.yaw_rate) * in.dt;
      if (turn180_t_ > 6.6 || mph < t180_min) {
        turn180_active_ = false;
      } else if (turn180_yaw_ >= t180_ang && std::fabs(fwd_mph) >= t180_end) {
        const auto [g, pts] = best_grade_lte(data_.turn180, turn180_t_);
        if (g > 0) {
          queue_award(kTurn180, "Turn180", std::string(grade_prefix(g)) + "180", pts, g, false, in.time, out);
          turn180_cd_ = in.time + std::max(2.0, data_.turn180.retrigger_delay);
        }
        turn180_active_ = false;
      }
    }

    const double jt_start = data_.jturn.param_a > 0 ? data_.jturn.param_a : 20.0;
    const double jt_end = data_.jturn.param_b > 0 ? data_.jturn.param_b : 20.0;
    const double jt_ang = (data_.jturn.param_c > 0 ? data_.jturn.param_c : 120.0) / kRadToDeg;
    if (!jturn_active_) {
      if (fwd_mph <= -jt_start && std::fabs(in.yaw_rate) >= 1.2 && in.time >= jturn_cd_) {
        jturn_active_ = true;
        jturn_t_ = 0.0;
        jturn_yaw_ = 0.0;
      }
    } else {
      jturn_t_ += in.dt;
      jturn_yaw_ += std::fabs(in.yaw_rate) * in.dt;
      if (jturn_t_ > 5.2) {
        jturn_active_ = false;
      } else if (jturn_yaw_ >= jt_ang && fwd_mph >= jt_end) {
        const auto [g, pts] = best_grade_lte(data_.jturn, jturn_t_);
        if (g > 0) {
          queue_award(kJTurn, "JTurn", std::string(grade_prefix(g)) + "J-TURN", pts, g, false, in.time, out);
          jturn_cd_ = in.time + std::max(2.0, data_.jturn.retrigger_delay);
        }
        jturn_active_ = false;
      }
    }
  }

  // 5. Air: all wheels off ground.
  {
    if (in.wheels_on_ground == 0) {
      air_t_ += in.dt;
    } else if (air_t_ > 0) {
      if (in.time >= air_cd_) {
        const auto [g, pts] = best_grade(data_.air, air_t_);
        if (g > 0) {
          queue_award(kAir, "Air", std::string(grade_prefix(g)) + "AIR", pts, g, false, in.time, out);
          air_cd_ = in.time + data_.air.retrigger_delay;
        }
      }
      air_t_ = 0.0;
    }
  }

  // 6. TwoWheels: exactly 2 wheels on ground at >= 45 mph.
  {
    const double min_spd = data_.two_wheels.param_a > 0 ? data_.two_wheels.param_a : 45.0;
    if (in.wheels_on_ground == 2 && mph >= min_spd) {
      two_wheels_t_ += in.dt;
    } else if (two_wheels_t_ > 0) {
      if (in.time >= two_wheels_cd_) {
        const auto [g, pts] = best_grade(data_.two_wheels, two_wheels_t_);
        if (g > 0) {
          queue_award(kTwoWheels, "TwoWheels", std::string(grade_prefix(g)) + "TWO WHEELS", pts, g, false, in.time, out);
          two_wheels_cd_ = in.time + std::max(1.0, data_.two_wheels.retrigger_delay);
        }
      }
      two_wheels_t_ = 0.0;
    }
  }

  // 7. Drift / 8. E-Drift: slip angle 15..110 deg above MinSpeed 25 mph.
  {
    const double slip_deg =
        in.speed_ms > 2.0 ? std::atan2(std::fabs(in.lateral_speed_ms), std::max(0.5, std::fabs(in.forward_speed_ms))) * kRadToDeg
                          : 0.0;
    const double min_spd = data_.drift.param_a > 0 ? data_.drift.param_a : 25.0;
    const double min_ang = data_.drift.param_b > 0 ? data_.drift.param_b : 15.0;
    const double max_ang = data_.drift.param_c > 0 ? data_.drift.param_c : 110.0;
    const double min_hb = data_.edrift.param_d > 0 ? data_.edrift.param_d : 1.0;
    const bool sliding = in.wheels_on_ground >= 2 && mph >= min_spd && slip_deg >= min_ang && slip_deg <= max_ang;
    if (sliding) {
      drift_dist_ += std::fabs(in.speed_ms) * in.dt;
      if (in.handbrake) drift_hb_t_ += in.dt;
      if (drift_hb_t_ >= min_hb) drift_is_e_ = true;
    } else if (drift_dist_ > 0) {
      if (in.time >= drift_cd_) {
        const auto& cfg = drift_is_e_ && !data_.edrift.grades.empty() ? data_.edrift : data_.drift;
        const auto [g, pts] = best_grade(cfg, drift_dist_);
        if (g > 0) {
          queue_award(drift_is_e_ ? kEDrift : kDrift, drift_is_e_ ? "EDrift" : "Drift",
                      std::string(grade_prefix(g)) + (drift_is_e_ ? "E-DRIFT" : "DRIFT"), pts, g, false, in.time, out);
          drift_cd_ = in.time + std::max(0.5, cfg.retrigger_delay);
        }
      }
      drift_dist_ = 0.0;
      drift_hb_t_ = 0.0;
      drift_is_e_ = false;
    }
  }

  // 9. Draft: draft factor > 0.1 at > 50 mph.
  {
    const double min_spd = data_.draft.param_a > 0 ? data_.draft.param_a : 50.0;
    const double min_df = data_.draft.param_b > 0 ? data_.draft.param_b : 0.1;
    if (in.draft_factor >= min_df && mph >= min_spd) {
      draft_t_ += in.dt;
    } else if (draft_t_ > 0) {
      if (in.time >= draft_cd_) {
        const auto [g, pts] = best_grade(data_.draft, draft_t_);
        if (g > 0) {
          queue_award(kDraft, "Draft", std::string(grade_prefix(g)) + "DRAFT", pts, g, false, in.time, out);
          draft_cd_ = in.time + std::max(1.0, data_.draft.retrigger_delay);
        }
      }
      draft_t_ = 0.0;
    }
  }

  // 10. Clean Speed: > 100 mph, one award per 450 m.
  {
    const double min_spd = data_.clean_speed.param_a > 0 ? data_.clean_speed.param_a : 100.0;
    const double step_dist = data_.clean_speed.param_b > 0 ? data_.clean_speed.param_b : 450.0;
    if (mph >= min_spd && in.wheels_on_ground >= 2) {
      speed_dist_ += std::fabs(in.speed_ms) * in.dt;
      speed_peak_mph_ = std::max(speed_peak_mph_, mph);
      if (speed_dist_ >= step_dist) {
        const auto [g, pts] = best_grade(data_.clean_speed, speed_peak_mph_);
        if (g > 0) queue_award(kCleanSpeed, "CleanSpeed", std::string(grade_prefix(g)) + "SPEED", pts, g, false, in.time, out);
        speed_dist_ = 0.0;
        speed_peak_mph_ = mph;
      }
    } else {
      speed_dist_ = 0.0;
      speed_peak_mph_ = 0.0;
    }
  }

  // 11. Near Miss & 12. Trading Paint
  {
    const double paint_spd = data_.trading_paint.param_a > 0 ? data_.trading_paint.param_a : 50.0;
    const double paint_d = data_.trading_paint.param_b > 0 ? data_.trading_paint.param_b : 3.2;
    if (in.car_impact_ms >= 0.4 && in.car_impact_ms < data_.car_collision_threshold && mph >= paint_spd &&
        in.nearest_car_dist <= paint_d + 1.5 && in.time >= paint_cd_) {
      queue_award(kTradingPaint, "TradingPaint", "TRADING PAINT", 50, 1, false, in.time, out);
      paint_cd_ = in.time + 2.0;
    }
    const double min_spd = data_.near_miss.param_a > 0 ? data_.near_miss.param_a : 50.0;
    const double max_d = data_.near_miss.param_b > 0 ? data_.near_miss.param_b : 4.0;
    const bool close = in.nearest_car_dist <= max_d && mph >= min_spd && in.car_impact_ms < 0.4;
    if (close && !in_near_miss_ && in.time >= near_miss_cd_) {
      const auto [g, pts] = best_grade(data_.near_miss, mph);
      if (g > 0) {
        queue_award(kNearMiss, "NearMiss", std::string(grade_prefix(g)) + "NEAR MISS", pts, g, false, in.time, out);
        near_miss_cd_ = in.time + std::max(0.8, data_.near_miss.retrigger_delay);
      }
    }
    in_near_miss_ = close;
  }

  // 13. Pass & 14. Sketchy Overtake (pass within PenaltyAgeThreshold of a collision)
  if (in.passed_opponent && in.time >= pass_cd_) {
    const double pen_age = data_.sketchy_overtake.param_a > 0 ? data_.sketchy_overtake.param_a : 4.0;
    if (in.time - last_collision_t_ <= pen_age) {
      queue_award(kSketchyOvertake, "SketchyOvertake", "SKETCHY OVERTAKE", 50, 1, false, in.time, out);
    } else {
      const auto [g, pts] = best_grade(data_.pass, std::max(0.0, in.pass_speed_diff_mph));
      const int use_g = g > 0 ? g : 1;
      const int use_pts = g > 0 ? pts : 100;
      queue_award(kPass, "Pass", std::string(grade_prefix(use_g)) + "PASS", use_pts, use_g, false, in.time, out);
    }
    pass_cd_ = in.time + std::max(0.8, data_.pass.retrigger_delay);
  }

  // 15. Destruction + 7 Smashable Object Specials (Landscaping, GoingPostal, RoadOpen, Trashman, Lumberjack, FruitSalad, Waterworks)
  if (in.smashed_object) {
    const double win = data_.destruction.param_a > 0 ? data_.destruction.param_a : 4.0;
    if (in.time - destruct_last_t_ > win) destruct_count_ = 0;
    ++destruct_count_;
    destruct_last_t_ = in.time;
    const auto [g, pts] = best_grade(data_.destruction, double(destruct_count_));
    const int use_g = g > 0 ? g : 1;
    const int use_pts = g > 0 ? pts : 100;
    queue_award(kDestruction, "Destruction", std::string(grade_prefix(use_g)) + "WRECKAGE", use_pts, use_g, false, in.time, out);

    if (const char* sid = special_id_for(in.smashed_type)) {
      const double cd = (data_.find_skill(sid) && data_.find_skill(sid)->retrigger_delay > 0)
                            ? data_.find_skill(sid)->retrigger_delay
                            : 1.0;
      if (in.time >= special_cd_[sid]) {
        special_cd_[sid] = in.time + cd;
        queue_award(kSpecial, sid, special_label_for(in.smashed_type), 1000, 4, false, in.time, out);
      }
    }
  }

  return out;
}

SkillHudState SkillTracker::hud() const {
  SkillHudState h;
  h.active = chain_points_ > 0;
  h.chain_points = chain_points_;
  h.multiplier = skill_count_ >= 6 ? 3 : skill_count_ >= 3 ? 2 : 1;
  h.skill_count = skill_count_;
  h.last_skill = last_skill_;
  h.last_points = last_points_;
  h.last_skill_at = last_award_at_;
  h.banked_total = banked_total_;
  h.banked_at = banked_at_;
  h.lost_at = lost_at_;
  h.popularity_points = popularity_;
  h.popularity_rank = data_.rank_for_points(popularity_);
  h.popularity_progress = data_.progress_in_rank(popularity_);
  h.rank_up_at = rank_up_at_;
  return h;
}

}  // namespace freeroam
