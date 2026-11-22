// SPEC-SKILLS-POPULARITY: driving skill detectors, skill chain + multiplier,
// banking/cancel rules, Popularity Rank ladder (Fame.xml, 250th -> 1st), and
// Fame sponsor credit payouts (SponsorshipChallenges.xml).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace freeroam {

struct FameLevel {
  int rank = 250;       // 249 down to 1
  double target = 0.0;  // cumulative popularity points
};

struct FameSponsorTier {
  int rank_required = 225;
  int64_t credits = 5000;
};

struct SkillGrade {
  std::string loc_string;  // e.g. "IDS_Drift3"
  double threshold = 0.0;  // time (s), distance (m), speed (mph), or count
  int points = 100;
};

struct SkillConfig {
  double retrigger_delay = 0.0;
  double param_a = 0.0;  // MinSpeed / SpeedThreshold / WheelSpinThreshold
  double param_b = 0.0;  // MinAngle / MaxSpeed
  double param_c = 0.0;  // MaxAngle
  std::vector<SkillGrade> grades;  // best first (grade 4 -> grade 1)
};

struct SkillsData {
  double world_collision_threshold = 12.0;  // m/s impact delta
  double car_collision_threshold = 12.0;
  double min_speed_to_cancel = 10.0;  // mph
  double delay_after_cancel = 3.0;    // s
  double tag_award_interval = 0.3;    // s
  double bank_timeout = 3.0;          // s without a new skill -> bank

  SkillConfig burnout, air, drift, edrift, draft, clean_speed, near_miss, pass, destruction;
  std::vector<FameLevel> fame_levels;        // sorted ascending by target (rank 249 -> 1)
  std::vector<FameSponsorTier> fame_tiers;   // sorted descending by rank_required (225 -> 1)

  // Current popularity rank (250 down to 1) for cumulative `points`.
  int rank_for_points(double points) const;
  // Fraction [0..1] progress from current rank's threshold to the next rank's.
  double progress_in_rank(double points) const;
  // Points needed for the next rank (0 if already rank 1).
  double next_rank_points(double points) const;
};

// Parses Fame.xml, HorizonFeats.xml, and SponsorshipChallenges.xml.
bool parse_skills_data(std::string_view fame_xml, std::string_view feats_xml, std::string_view sponsors_xml,
                       SkillsData& out, std::string* error = nullptr);
// Loads from `<game_root>/media/gametunablesettings.zip`.
bool load_skills_data(const std::filesystem::path& game_root, SkillsData& out, std::string* error = nullptr);

// Per-physics-step input to the skill tracker.
struct SkillInput {
  double dt = 0.0;
  double time = 0.0;              // drive clock (s)
  double speed_ms = 0.0;          // total speed magnitude (m/s)
  double forward_speed_ms = 0.0;  // signed forward speed (m/s)
  double lateral_speed_ms = 0.0;  // lateral speed (m/s)
  double wheel_spin = 0.0;        // max driven wheel slip speed (m/s)
  bool handbrake = false;
  int wheels_on_ground = 4;       // 0..4
  double world_impact_ms = 0.0;   // estimated world collision impulse / mass (m/s)
  double car_impact_ms = 0.0;     // estimated car collision impulse / mass (m/s)
  double nearest_car_dist = 1e9;  // distance to closest traffic/opponent car (m)
  double draft_factor = 0.0;      // [0..1] slipstream behind another car
  bool passed_opponent = false;   // overtook an opponent this step
  double pass_speed_diff_mph = 0.0;
  bool smashed_object = false;    // smashed a sign/smashable this step
};

struct SkillEvent {
  enum Kind { kSkillAwarded, kChainBanked, kChainLost, kRankUp, kFameSponsor };
  Kind kind = kSkillAwarded;
  std::string label;
  int points = 0;
  int multiplier = 1;
  int rank = 250;
  int64_t credits = 0;
};

struct SkillHudState {
  bool active = false;
  int chain_points = 0;
  int multiplier = 1;
  int skill_count = 0;
  std::string last_skill;
  int last_points = 0;
  double last_skill_at = -1e9;
  int banked_total = 0;
  double banked_at = -1e9;
  double lost_at = -1e9;
  int popularity_rank = 250;
  double popularity_progress = 0.0;  // [0..1] for radial bar
  double popularity_points = 0.0;
  double rank_up_at = -1e9;
};

class SkillTracker {
 public:
  SkillTracker(const SkillsData& data, double initial_popularity, int initial_fame_claimed, int64_t best_score = 0);

  std::vector<SkillEvent> step(const SkillInput& in);
  // Resets an active chain cleanly (e.g. entering a menu/race).
  void clear_active_chain();
  // Resets all session-clock timestamps/cooldowns across vehicle reloads.
  void reset_session_clock();

  SkillHudState hud() const;
  double popularity_points() const { return popularity_; }
  void add_popularity(double pts) { if (pts > 0.0) popularity_ += pts; }
  int popularity_rank() const { return data_.rank_for_points(popularity_); }
  int fame_rewards_claimed() const { return fame_claimed_; }
  int64_t max_skills_score() const { return max_score_; }

 private:
  enum SkillId {
    kBurnout,
    kAir,
    kDrift,
    kEDrift,
    kDraft,
    kCleanSpeed,
    kNearMiss,
    kPass,
    kDestruction,
    kCombo,
    kCount
  };
  void award(SkillId id, const std::string& label, int points, int grade, double time, std::vector<SkillEvent>& out);
  void check_combos(double time, std::vector<SkillEvent>& out);
  void bank_chain(double time, std::vector<SkillEvent>& out);
  void cancel_chain(double time, std::vector<SkillEvent>& out);

  const SkillsData& data_;
  double popularity_ = 0.0;
  int fame_claimed_ = 0;
  int64_t max_score_ = 0;

  // Chain state.
  int chain_points_ = 0;
  int skill_count_ = 0;
  double last_award_at_ = -1e9;
  double lockout_until_ = -1e9;
  bool over9000_paid_ = false;

  // Recent HUD display.
  std::string last_skill_;
  int last_points_ = 0;
  int banked_total_ = 0;
  double banked_at_ = -1e9;
  double lost_at_ = -1e9;
  double rank_up_at_ = -1e9;

  // Detector accumulators.
  double burnout_t_ = 0.0, burnout_cd_ = -1e9;
  double air_t_ = 0.0, air_cd_ = -1e9;
  double drift_dist_ = 0.0, drift_hb_t_ = 0.0;
  bool drift_is_e_ = false;
  double drift_cd_ = -1e9;
  double draft_t_ = 0.0, draft_cd_ = -1e9;
  double speed_dist_ = 0.0, speed_peak_mph_ = 0.0;
  bool in_near_miss_ = false;
  double near_miss_cd_ = -1e9;
  double pass_cd_ = -1e9;
  int destruct_count_ = 0;
  double destruct_last_t_ = -1e9;

  struct RecentSkill {
    SkillId id;
    int grade;
    double time;
  };
  std::vector<RecentSkill> recent_;
  std::vector<std::pair<std::string, double>> combo_cd_;
};

}  // namespace freeroam
