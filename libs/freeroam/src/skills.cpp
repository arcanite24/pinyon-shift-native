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

void read_grades(const audio::XmlNode* node, const char* measure_attr, SkillConfig& out) {
  if (!node) return;
  out.retrigger_delay = node->number("RetriggerDelay", 0.0);
  out.grades.clear();
  for (const auto& ch : node->children) {
    if (!audio::iequals(ch.name, "Grade")) continue;
    SkillGrade g;
    g.loc_string = ch.attr("LocString");
    g.threshold = ch.number(measure_attr, 0.0);
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
  // Grades are stored best-first (index 0 -> grade 4, ..., index 3 -> grade 1).
  for (size_t i = 0; i < cfg.grades.size(); ++i) {
    if (measure >= cfg.grades[i].threshold) {
      const int grade = int(cfg.grades.size() - i);
      return {grade, cfg.grades[i].points};
    }
  }
  return {0, 0};
}

}  // namespace

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
  out.tag_award_interval = feats.number("TagAwardInterval", 0.3);

  if (const auto* n = feats.child("Burnout")) {
    read_grades(n, "Time", out.burnout);
    out.burnout.param_a = n->number("WheelSpinThreshold", 10.0);
    out.burnout.param_b = n->number("MaxSpeed", 5.0);
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
  }
  if (const auto* n = feats.child("Draft")) {
    read_grades(n, "Time", out.draft);
    out.draft.param_a = n->number("MinSpeed", 50.0);
    out.draft.param_b = n->number("MinDraftFactor", 0.1);
  }
  if (const auto* n = feats.child("CleanSpeed")) {
    read_grades(n, "Speed", out.clean_speed);
    out.clean_speed.param_a = n->number("SpeedThreshold", 100.0);
    out.clean_speed.param_b = n->number("DistancePerAward", 450.0);
  }
  if (const auto* n = feats.child("NearMiss")) {
    read_grades(n, "Speed", out.near_miss);
    out.near_miss.param_a = n->number("MinSpeed", 50.0);
    out.near_miss.param_b = n->number("MaxDistance", 4.0);
  }
  if (const auto* n = feats.child("Pass")) {
    read_grades(n, "SpeedDiff", out.pass);
    out.pass.param_a = n->number("PassResetTime", 4.0);
  }
  if (const auto* n = feats.child("Destruction")) {
    read_grades(n, "Objects", out.destruction);
    out.destruction.param_a = n->number("ChainWindow", 4.0);
  }

  for (const auto& ch : sponsors.children) {
    if (!audio::iequals(ch.name, "Challenge") || !audio::iequals(ch.attr("type"), "FameRankChallenge")) continue;
    for (const auto& rk : ch.children) {
      if (!audio::iequals(rk.name, "Rank")) continue;
      FameSponsorTier t;
      t.rank_required = int(rk.number("levelRequired", 225));
      t.credits = int64_t(std::llround(rk.number("credits", 0)));
      if (t.credits > 0) out.fame_tiers.push_back(t);
    }
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
                           int64_t best_score)
    : data_(data), popularity_(initial_popularity), fame_claimed_(initial_fame_claimed), max_score_(best_score) {}

void SkillTracker::clear_active_chain() {
  chain_points_ = 0;
  skill_count_ = 0;
  over9000_paid_ = false;
  burnout_t_ = air_t_ = drift_dist_ = drift_hb_t_ = draft_t_ = speed_dist_ = speed_peak_mph_ = 0.0;
  drift_is_e_ = false;
  destruct_count_ = 0;
  recent_.clear();
}

void SkillTracker::reset_session_clock() {
  clear_active_chain();
  last_award_at_ = lockout_until_ = -1e9;
  banked_at_ = lost_at_ = rank_up_at_ = -1e9;
  burnout_cd_ = air_cd_ = drift_cd_ = draft_cd_ = near_miss_cd_ = pass_cd_ = destruct_last_t_ = -1e9;
  in_near_miss_ = false;
  combo_cd_.clear();
  last_skill_.clear();
  last_points_ = 0;
  banked_total_ = 0;
}

void SkillTracker::award(SkillId id, const std::string& label, int points, int grade, double time,
                         std::vector<SkillEvent>& out) {
  if (points <= 0 || time < lockout_until_) return;
  chain_points_ += points;
  ++skill_count_;
  last_award_at_ = time;
  last_skill_ = label;
  last_points_ = points;
  const int mult = skill_count_ >= 6 ? 3 : skill_count_ >= 3 ? 2 : 1;
  out.push_back({SkillEvent::kSkillAwarded, label, points, mult, popularity_rank(), 0});
  if (id != kCombo) {
    recent_.push_back({id, grade, time});
    // Prune older than 36 s.
    recent_.erase(std::remove_if(recent_.begin(), recent_.end(), [&](const RecentSkill& r) { return time - r.time > 36.0; }),
                  recent_.end());
    check_combos(time, out);
  }
  if (!over9000_paid_ && chain_points_ >= 9000) {
    over9000_paid_ = true;
    chain_points_ += 1000;
    ++skill_count_;
    last_skill_ = "OVER 9000!";
    last_points_ = 1000;
    const int m2 = skill_count_ >= 6 ? 3 : skill_count_ >= 3 ? 2 : 1;
    out.push_back({SkillEvent::kSkillAwarded, last_skill_, 1000, m2, popularity_rank(), 0});
  }
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

  if (count_recent(kNearMiss, 10.0) >= 3 && ready("DAREDEVIL", 10.0))
    award(kCombo, "DAREDEVIL", 1000, 1, time, out);
  if (count_recent(kAir, 5.0) >= 3 && ready("KANGAROO", 5.0))
    award(kCombo, "KANGAROO", 1000, 1, time, out);
  if (count_recent(kPass, 2.0) >= 3 && ready("TRIPLE PASS", 2.0))
    award(kCombo, "TRIPLE PASS", 1000, 1, time, out);
  if (has_pair(kAir, kNearMiss, 2.5, false) && ready("STUNTMAN", 3.0))
    award(kCombo, "STUNTMAN", 1000, 1, time, out);
  if (has_pair(kAir, kPass, 2.5, false) && ready("SUPERMAN", 3.0))
    award(kCombo, "SUPERMAN", 1000, 1, time, out);
  if (has_pair(kBurnout, kCleanSpeed, 35.0, true) && ready("QUICK OFF THE MARK", 35.0))
    award(kCombo, "QUICK OFF THE MARK", 1000, 1, time, out);
  if (has_pair(kDraft, kPass, 5.0, true) && ready("SLINGSHOT", 5.0))
    award(kCombo, "SLINGSHOT", 1000, 1, time, out);
  if ((has_pair(kDrift, kPass, 2.5, true) || has_pair(kEDrift, kPass, 2.5, true)) && ready("SHOW OFF", 3.0))
    award(kCombo, "SHOW OFF", 1000, 1, time, out);
  if ((has_pair(kDrift, kAir, 0.5, false, 2) || has_pair(kEDrift, kAir, 0.5, false, 2)) && ready("EBISU STYLE", 3.0))
    award(kCombo, "EBISU STYLE", 1000, 1, time, out);
  if ((has_pair(kDrift, kNearMiss, 2.5, true) || has_pair(kEDrift, kNearMiss, 2.5, true)) && ready("LUCKY ESCAPE", 3.0))
    award(kCombo, "LUCKY ESCAPE", 1000, 1, time, out);
}

void SkillTracker::bank_chain(double time, std::vector<SkillEvent>& out) {
  if (chain_points_ <= 0) return;
  const int mult = skill_count_ >= 6 ? 3 : skill_count_ >= 3 ? 2 : 1;
  const int total = chain_points_ * mult;
  const int rank_before = popularity_rank();
  popularity_ += double(total);
  const int rank_after = popularity_rank();
  if (total > max_score_) max_score_ = total;
  banked_total_ = total;
  banked_at_ = time;
  out.push_back({SkillEvent::kChainBanked, "SKILL CHAIN BANKED", total, mult, rank_after, 0});
  if (rank_after < rank_before) {
    rank_up_at_ = time;
    out.push_back({SkillEvent::kRankUp, "POPULARITY RANK UP", 0, 1, rank_after, 0});
    // Check Fame sponsor credit tiers.
    for (size_t i = size_t(std::max(0, fame_claimed_)); i < data_.fame_tiers.size(); ++i) {
      if (rank_after <= data_.fame_tiers[i].rank_required) {
        fame_claimed_ = int(i + 1);
        out.push_back({SkillEvent::kFameSponsor, "FAME SPONSOR BONUS", 0, 1, data_.fame_tiers[i].rank_required,
                       data_.fame_tiers[i].credits});
      }
    }
  }
  clear_active_chain();
}

void SkillTracker::cancel_chain(double time, std::vector<SkillEvent>& out) {
  if (chain_points_ > 0) {
    lost_at_ = time;
    out.push_back({SkillEvent::kChainLost, "SKILL CHAIN LOST", chain_points_, 1, popularity_rank(), 0});
  }
  clear_active_chain();
  lockout_until_ = time + data_.delay_after_cancel;
}

std::vector<SkillEvent> SkillTracker::step(const SkillInput& in) {
  std::vector<SkillEvent> out;
  if (in.dt <= 0) return out;
  const double mph = std::fabs(in.speed_ms) * kMsToMph;

  // Cancel on hard collision (SPEC-SKILLS-POPULARITY: World/CarCollisionThreshold 12).
  if (in.world_impact_ms >= data_.world_collision_threshold || in.car_impact_ms >= data_.car_collision_threshold) {
    if (chain_points_ > 0 || burnout_t_ > 0 || drift_dist_ > 0 || air_t_ > 0) cancel_chain(in.time, out);
    return out;
  }

  // Bank active chain after timeout without a new skill, or cancel if stopped (except while doing a burnout).
  if (chain_points_ > 0) {
    if (mph < data_.min_speed_to_cancel && burnout_t_ <= 0.1 && in.time - last_award_at_ > 1.0) {
      bank_chain(in.time, out);
    } else if (in.time - last_award_at_ >= data_.bank_timeout && drift_dist_ <= 1.0 && air_t_ <= 0.05 &&
               burnout_t_ <= 0.1 && draft_t_ <= 0.1) {
      bank_chain(in.time, out);
    }
  }

  if (in.time < lockout_until_) return out;

  // 1. Burnout: wheel spin > threshold at low speed (< MaxSpeed).
  {
    const bool spinning = in.wheels_on_ground >= 2 && in.wheel_spin >= data_.burnout.param_a &&
                          mph <= std::max(5.0, data_.burnout.param_b * kMsToMph);
    if (spinning) {
      burnout_t_ += in.dt;
    } else if (burnout_t_ > 0) {
      if (in.time >= burnout_cd_) {
        const auto [g, pts] = best_grade(data_.burnout, burnout_t_);
        if (g > 0) {
          award(kBurnout, std::string(grade_prefix(g)) + "BURNOUT", pts, g, in.time, out);
          burnout_cd_ = in.time + data_.burnout.retrigger_delay;
        }
      }
      burnout_t_ = 0.0;
    }
  }

  // 2. Air: all wheels off ground.
  {
    if (in.wheels_on_ground == 0) {
      air_t_ += in.dt;
    } else if (air_t_ > 0) {
      if (in.time >= air_cd_) {
        const auto [g, pts] = best_grade(data_.air, air_t_);
        if (g > 0) {
          award(kAir, std::string(grade_prefix(g)) + "AIR", pts, g, in.time, out);
          air_cd_ = in.time + data_.air.retrigger_delay;
        }
      }
      air_t_ = 0.0;
    }
  }

  // 3. Drift / E-Drift: slip angle 15..110 deg above MinSpeed 25 mph.
  {
    const double slip_deg =
        in.speed_ms > 2.0 ? std::atan2(std::fabs(in.lateral_speed_ms), std::max(0.5, std::fabs(in.forward_speed_ms))) * kRadToDeg
                          : 0.0;
    const double min_spd = data_.drift.param_a > 0 ? data_.drift.param_a : 25.0;
    const double min_ang = data_.drift.param_b > 0 ? data_.drift.param_b : 15.0;
    const double max_ang = data_.drift.param_c > 0 ? data_.drift.param_c : 110.0;
    const bool sliding = in.wheels_on_ground >= 2 && mph >= min_spd && slip_deg >= min_ang && slip_deg <= max_ang;
    if (sliding) {
      drift_dist_ += std::fabs(in.speed_ms) * in.dt;
      if (in.handbrake) drift_hb_t_ += in.dt;
      if (drift_hb_t_ >= 1.0) drift_is_e_ = true;
    } else if (drift_dist_ > 0) {
      if (in.time >= drift_cd_) {
        const auto& cfg = drift_is_e_ && !data_.edrift.grades.empty() ? data_.edrift : data_.drift;
        const auto [g, pts] = best_grade(cfg, drift_dist_);
        if (g > 0) {
          award(drift_is_e_ ? kEDrift : kDrift,
                std::string(grade_prefix(g)) + (drift_is_e_ ? "E-DRIFT" : "DRIFT"), pts, g, in.time, out);
          drift_cd_ = in.time + std::max(0.5, cfg.retrigger_delay);
        }
      }
      drift_dist_ = 0.0;
      drift_hb_t_ = 0.0;
      drift_is_e_ = false;
    }
  }

  // 4. Draft: draft factor > 0.1 at > 50 mph.
  {
    const double min_spd = data_.draft.param_a > 0 ? data_.draft.param_a : 50.0;
    const double min_df = data_.draft.param_b > 0 ? data_.draft.param_b : 0.1;
    if (in.draft_factor >= min_df && mph >= min_spd) {
      draft_t_ += in.dt;
    } else if (draft_t_ > 0) {
      if (in.time >= draft_cd_) {
        const auto [g, pts] = best_grade(data_.draft, draft_t_);
        if (g > 0) {
          award(kDraft, std::string(grade_prefix(g)) + "DRAFT", pts, g, in.time, out);
          draft_cd_ = in.time + std::max(1.0, data_.draft.retrigger_delay);
        }
      }
      draft_t_ = 0.0;
    }
  }

  // 5. Clean Speed: > 100 mph, one award per 450 m.
  {
    const double min_spd = data_.clean_speed.param_a > 0 ? data_.clean_speed.param_a : 100.0;
    const double step_dist = data_.clean_speed.param_b > 0 ? data_.clean_speed.param_b : 450.0;
    if (mph >= min_spd && in.wheels_on_ground >= 2) {
      speed_dist_ += std::fabs(in.speed_ms) * in.dt;
      speed_peak_mph_ = std::max(speed_peak_mph_, mph);
      if (speed_dist_ >= step_dist) {
        const auto [g, pts] = best_grade(data_.clean_speed, speed_peak_mph_);
        if (g > 0) award(kCleanSpeed, std::string(grade_prefix(g)) + "SPEED", pts, g, in.time, out);
        speed_dist_ = 0.0;
        speed_peak_mph_ = mph;
      }
    } else {
      speed_dist_ = 0.0;
      speed_peak_mph_ = 0.0;
    }
  }

  // 6. Near Miss: another car within 4 m at >= 50 mph without colliding.
  {
    const double min_spd = data_.near_miss.param_a > 0 ? data_.near_miss.param_a : 50.0;
    const double max_d = data_.near_miss.param_b > 0 ? data_.near_miss.param_b : 4.0;
    const bool close = in.nearest_car_dist <= max_d && mph >= min_spd;
    if (close && !in_near_miss_ && in.time >= near_miss_cd_) {
      const auto [g, pts] = best_grade(data_.near_miss, mph);
      if (g > 0) {
        award(kNearMiss, std::string(grade_prefix(g)) + "NEAR MISS", pts, g, in.time, out);
        near_miss_cd_ = in.time + std::max(0.8, data_.near_miss.retrigger_delay);
      }
    }
    in_near_miss_ = close;
  }

  // 7. Pass: overtaking an opponent in a race.
  if (in.passed_opponent && in.time >= pass_cd_) {
    const auto [g, pts] = best_grade(data_.pass, std::max(0.0, in.pass_speed_diff_mph));
    const int use_g = g > 0 ? g : 1;
    const int use_pts = g > 0 ? pts : 100;
    award(kPass, std::string(grade_prefix(use_g)) + "PASS", use_pts, use_g, in.time, out);
    pass_cd_ = in.time + std::max(0.8, data_.pass.retrigger_delay);
  }

  // 8. Destruction: smashing signs / objects chained within 4 s.
  if (in.smashed_object) {
    const double win = data_.destruction.param_a > 0 ? data_.destruction.param_a : 4.0;
    if (in.time - destruct_last_t_ > win) destruct_count_ = 0;
    ++destruct_count_;
    destruct_last_t_ = in.time;
    const auto [g, pts] = best_grade(data_.destruction, double(destruct_count_));
    const int use_g = g > 0 ? g : 1;
    const int use_pts = g > 0 ? pts : 100;
    award(kDestruction, std::string(grade_prefix(use_g)) + "WRECKAGE", use_pts, use_g, in.time, out);
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
