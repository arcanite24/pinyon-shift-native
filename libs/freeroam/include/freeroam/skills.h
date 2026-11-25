// SPEC-SKILLS-POPULARITY: full 54/54 HorizonFeats driving skill detectors,
// skill chain + multiplier, banking/cancel rules (MinSpeedToCancelFeats,
// TagAwardInterval, MaxSkillsCarOrdinal), Popularity Rank ladder (Fame.xml,
// 250th -> 1st), and all 14 SponsorshipChallenges.xml categories + tiered
// credit payouts.
#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace freeroam {

struct FameLevel {
  int rank = 250;                    // 249 down to 1
  double target = 0.0;               // cumulative popularity points
  std::string dialogue_event;        // Fame.xml dialogueEvent trigger (empty if none)
  std::string radio_festival_update; // Fame.xml radioFestivalUpdate trigger (empty if none)
};

struct FameSponsorTier {
  int rank_required = 225;
  int64_t credits = 5000;
};

struct SkillSponsorTier {
  std::string skill;  // e.g. "Air", "AnySkill", "Daredevil"
  int grade = 4;      // 0 = Ultimate, 1 = Awesome, 2 = Great, 3/4 = Any
  int number = 25;    // count required
  int64_t credits = 5000;
};

struct SponsorChallenge {
  std::string id;
  std::string type;  // "FameRankChallenge" or "SkillChallenge"
  std::string sponsor_name;
  std::string challenge_name;
  std::string description;  // IDS_ name in SponsorshipChallenges (a "{0}" target format)
  std::string logo;         // the sponsorLogo texture path (SponsorLogoTile/*)
  std::vector<SkillSponsorTier> tiers;
};

struct SkillGrade {
  std::string loc_string;  // e.g. "IDS_Drift3"
  double threshold = 0.0;  // time (s), distance (m), speed (mph), or count
  int points = 100;
};

struct SkillConfig {
  std::string id;
  double retrigger_delay = 0.0;
  double param_a = 0.0;  // MinSpeed / SpeedThreshold / WheelSpinThreshold / MinSpeedToStart
  double param_b = 0.0;  // MinAngle / MaxSpeed / MetersPerAward / MinSpeedToEnd
  double param_c = 0.0;  // MaxAngle / MinAngularChange / ChainWindow
  double param_d = 0.0;  // MinSpeedDuring180 / ComboWindowDuration
  std::string object_type;  // for smashable specials (Fence, Mailbox, etc.)
  std::vector<SkillGrade> grades;  // best first (grade 4 -> grade 1)
};

struct SkillsData {
  double world_collision_threshold = 12.0;  // m/s impact delta
  double car_collision_threshold = 12.0;
  double min_speed_to_cancel = 10.0;  // mph
  double delay_after_cancel = 3.0;    // s
  double tag_award_interval = 0.3;    // s
  double bank_timeout = 3.0;          // s without a new skill -> bank

  SkillConfig burnout, donut, turn180, jturn, air, drift, edrift, draft, two_wheels;
  SkillConfig clean_speed, near_miss, sketchy_overtake, trading_paint, pass, destruction;
  // Complete 54/54 HorizonFeats registry (all base, online, combo, multiplier, special nodes).
  std::vector<SkillConfig> all_skills;
  std::vector<FameLevel> fame_levels;        // sorted ascending by target (rank 249 -> 1)
  std::vector<FameSponsorTier> fame_tiers;   // sorted descending by rank_required (225 -> 1)
  std::vector<SponsorChallenge> sponsor_challenges;  // all 14 SponsorshipChallenges.xml categories

  const SkillConfig* find_skill(std::string_view id) const;
  const FameLevel* find_fame_level(int rank) const;
  size_t skill_def_count() const { return all_skills.size(); }

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

// Special smashable object categories for the 7 object-specific skills.
enum class SmashObjectType : int {
  kGeneric = 0,
  kFence,          // Landscaping
  kMailbox,        // GoingPostal
  kRoadClosedSign, // RoadOpen
  kTrashCan,       // Trashman
  kLogPiles,       // Lumberjack
  kFruitCart,      // FruitSalad
  kFireHydrant,    // Waterworks
};

// Per-physics-step input to the skill tracker.
struct SkillInput {
  double dt = 0.0;
  double time = 0.0;              // drive clock (s)
  double speed_ms = 0.0;          // total speed magnitude (m/s)
  double forward_speed_ms = 0.0;  // signed forward speed (m/s)
  double lateral_speed_ms = 0.0;  // lateral speed (m/s)
  double yaw_rate = 0.0;          // rad/s (for Donut, 180, J-Turn)
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
  SmashObjectType smashed_type = SmashObjectType::kGeneric;
  int64_t car_id = 0;             // Data_Car.Id for MaxSkillsCarOrdinal
};

struct SkillEvent {
  enum Kind { kSkillAwarded, kChainBanked, kChainLost, kRankUp, kFameSponsor, kSponsorChallenge };
  Kind kind = kSkillAwarded;
  std::string label;
  std::string skill_id;   // canonical HorizonFeats / SponsorChallenge id
  int points = 0;
  int multiplier = 1;
  int rank = 250;
  int64_t credits = 0;
  int grade = 1;                     // 1=Standard, 2=Great, 3=Awesome, 4=Ultimate
  std::string dialogue_event;        // populated on kRankUp from crossed Fame.xml Level
  std::string radio_festival_update; // populated on kRankUp from crossed Fame.xml Level
  float multiplier_f = 1.0f;         // exact 1.0x..5.0x in 0.5x steps
};

struct SkillHudState {
  bool active = false;
  int chain_points = 0;
  int multiplier = 1;
  float multiplier_f = 1.0f;  // exact 1.0x..5.0x in 0.5x steps
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

struct SponsorProgress {
  int count = 0;
  int claimed_tiers = 0;
};

class SkillTracker {
 public:
  SkillTracker(const SkillsData& data, double initial_popularity, int initial_fame_claimed, int64_t best_score = 0,
               int64_t best_car = 0);

  std::vector<SkillEvent> step(const SkillInput& in);
  // Triggers any named HorizonFeats skill directly (e.g. online/playground skills).
  std::vector<SkillEvent> award_named(std::string_view skill_id, int grade, double time);
  // Resets an active chain cleanly (e.g. entering a menu/race).
  void clear_active_chain();
  // Resets all session-clock timestamps/cooldowns across vehicle reloads.
  void reset_session_clock();

  SkillHudState hud() const;
  float chain_multiplier() const;
  double popularity_points() const { return popularity_; }
  // Updates popularity and evaluates Popularity Rank transitions, Fame Sponsor
  // CR payouts, and Showcase unlock triggers identically to bank_chain().
  std::vector<SkillEvent> add_popularity(double pts, double time = -1e9);
  int popularity_rank() const { return data_.rank_for_points(popularity_); }
  int fame_rewards_claimed() const { return fame_claimed_; }
  int64_t max_skills_score() const { return max_score_; }
  int64_t max_skills_car() const { return max_car_; }
  int64_t race_skills_banked() const { return race_banked_; }
  void reset_race_skills_banked() { race_banked_ = 0; }

  const std::map<std::string, SponsorProgress>& sponsor_progress() const { return sponsors_; }
  void set_sponsor_progress(const std::string& id, int count, int claimed);

  // Remaining skill-chain bank timer in seconds at `now_s` (0 if no active chain).
  double chain_remaining_s(double now_s) const;

  // Full skill-chain & detector snapshot for Rewind rollback (preserves a live
  // combo across rewind instead of leaving it broken).
  struct RewindSnapshot {
    int chain_points = 0;
    int skill_count = 0;
    double last_award_at = -1e9;
    double last_tag_at = -1e9;
    double lockout_until = -1e9;
    bool over9000_paid = false;
    std::string last_skill;
    int last_points = 0;
    double lost_at = -1e9;
    double burnout_t = 0.0, burnout_cd = -1e9;
    double donut_yaw = 0.0, donut_dist = 0.0, donut_cd = -1e9;
    bool turn180_active = false;
    double turn180_t = 0.0, turn180_yaw = 0.0, turn180_cd = -1e9;
    bool jturn_active = false;
    double jturn_t = 0.0, jturn_yaw = 0.0, jturn_cd = -1e9;
    double air_t = 0.0, air_cd = -1e9;
    double two_wheels_t = 0.0, two_wheels_cd = -1e9;
    double drift_dist = 0.0, drift_hb_t = 0.0;
    bool drift_is_e = false;
    double drift_cd = -1e9;
    double draft_t = 0.0, draft_cd = -1e9;
    double speed_dist = 0.0, speed_peak_mph = 0.0;
    bool in_near_miss = false;
    double near_miss_cd = -1e9;
    double paint_cd = -1e9;
    double pass_cd = -1e9;
    double last_collision_t = -1e9;
    int destruct_count = 0;
    double destruct_last_t = -1e9;
  };
  RewindSnapshot capture_rewind() const;
  void restore_rewind(const RewindSnapshot& s);

 private:
  enum SkillId {
    kBurnout,
    kDonut,
    kTurn180,
    kJTurn,
    kAir,
    kDrift,
    kEDrift,
    kDraft,
    kTwoWheels,
    kCleanSpeed,
    kNearMiss,
    kSketchyOvertake,
    kTradingPaint,
    kPass,
    kDestruction,
    kCombo,
    kSpecial,
    kCount
  };
  struct PendingTag {
    SkillId id = kCombo;
    std::string canonical_id;
    std::string label;
    int points = 0;
    int grade = 1;
    bool is_combo = false;
  };
  struct PendingSponsorFeat {
    std::string canonical_id;
    int grade = 1;
    bool is_combo = false;
  };
  void queue_award(SkillId id, std::string canonical_id, std::string label, int points, int grade, bool is_combo,
                   double time, std::vector<SkillEvent>& out);
  void emit_tag(const PendingTag& tag, double time, std::vector<SkillEvent>& out);
  void flush_tags(double time, std::vector<SkillEvent>& out);
  void check_combos(double time, std::vector<SkillEvent>& out);
  void record_sponsor_skill(const std::string& canonical_id, int grade, bool is_combo, std::vector<SkillEvent>& out);
  void evaluate_popularity_transitions(int rank_before, double time, std::vector<SkillEvent>& out);
  void bank_chain(double time, std::vector<SkillEvent>& out);
  void cancel_chain(double time, std::vector<SkillEvent>& out);

  const SkillsData& data_;
  double popularity_ = 0.0;
  int fame_claimed_ = 0;
  int64_t max_score_ = 0;
  int64_t max_car_ = 0;
  int64_t current_car_ = 0;
  int64_t race_banked_ = 0;
  std::map<std::string, SponsorProgress> sponsors_;
  std::vector<PendingSponsorFeat> pending_sponsor_feats_;
  std::vector<SkillEvent> pending_pop_events_;
  double last_step_time_ = -1e9;

  // Chain state.
  int chain_points_ = 0;
  int skill_count_ = 0;
  double last_award_at_ = -1e9;
  double last_tag_at_ = -1e9;
  double lockout_until_ = -1e9;
  bool over9000_paid_ = false;
  std::deque<PendingTag> pending_tags_;

  // Recent HUD display.
  std::string last_skill_;
  int last_points_ = 0;
  int banked_total_ = 0;
  double banked_at_ = -1e9;
  double lost_at_ = -1e9;
  double rank_up_at_ = -1e9;

  // Detector accumulators.
  double burnout_t_ = 0.0, burnout_cd_ = -1e9;
  double donut_yaw_ = 0.0, donut_dist_ = 0.0, donut_cd_ = -1e9;
  // Turn180 & JTurn state
  bool turn180_active_ = false;
  double turn180_t_ = 0.0, turn180_yaw_ = 0.0, turn180_cd_ = -1e9;
  bool jturn_active_ = false;
  double jturn_t_ = 0.0, jturn_yaw_ = 0.0, jturn_cd_ = -1e9;
  double air_t_ = 0.0, air_cd_ = -1e9;
  double two_wheels_t_ = 0.0, two_wheels_cd_ = -1e9;
  double drift_dist_ = 0.0, drift_hb_t_ = 0.0;
  bool drift_is_e_ = false;
  double drift_cd_ = -1e9;
  double draft_t_ = 0.0, draft_cd_ = -1e9;
  double speed_dist_ = 0.0, speed_peak_mph_ = 0.0;
  bool in_near_miss_ = false;
  double near_miss_cd_ = -1e9;
  double paint_cd_ = -1e9;
  double pass_cd_ = -1e9;
  double last_collision_t_ = -1e9;
  int destruct_count_ = 0;
  double destruct_last_t_ = -1e9;
  std::map<std::string, double> special_cd_;

  struct RecentSkill {
    SkillId id;
    std::string canonical_id;
    int grade;
    double time;
  };
  std::vector<RecentSkill> recent_;
  std::vector<std::pair<std::string, double>> combo_cd_;
};

}  // namespace freeroam
