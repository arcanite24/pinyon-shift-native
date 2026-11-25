// Complete Forza Horizon 1 campaign progression state machine (Minute 0 through
// Horizon Festival Finale & Credits): onboarding flow, wristband milestones,
// all 7 Star Showdown boss rivals, 10 Showcases, Barn Find cinematics, voiced
// character dialog/subtitle queue with audio ducking envelope, UI4 screen
// transitions, animated post-race reward tally, and career save round-trip.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "race/career.h"
#include "race/rewards.h"

namespace drive {
struct DriveSave;
}

namespace race {

// ---- Smooth UI4 & Cinematic Easing Curves ----
double ease_out_cubic(double t);
double ease_in_out_cubic(double t);
double ease_out_back(double t);

// ---- Canonical Campaign Stages (Minute 0 -> Finale & Credits) ----
enum class CampaignStage : uint8_t {
  kTitle = 0,
  kOpeningCutscene,       // Minute 0: FMV_01 Festival opening montage
  kViperIntroDrive,       // Minute 0: 2013 SRT Viper GTS highway drive to Horizon Festival
  kQualifyRace,           // Minute 0: 1995 VW Corrado VR6 qualification race (FESTIVAL_FIRST / route 16)
  kFestivalRegistration,  // Minute 0: Alice/Dak orientation, Corrado handoff & Yellow Wristband ceremony
  kFreeRoam,              // Active Colorado free roam across Yellow..Gold wristbands
  kWristbandCeremony,     // Race Central wristband tier-up stage fly-in & unlock ceremony
  kShowcaseIntro,         // Showcase intro cinematic (aircraft / loaner hero reveal)
  kShowcaseOutro,         // Showcase victory/completion celebration
  kBossIntro,             // Star Showdown / Boss Rival intro card & camera sweep
  kBossVictory,           // Star Showdown / Boss Rival victory & pink-slip celebration
  kBarnDiscovery,         // Barn Find arrival camera reveal
  kBarnRestored,          // Dak Stewart workshop restoration handoff
  kPreRaceGrid,           // Pre-race grid cubic-bezier flyby & event lower-third
  kPostRaceFinish,        // Post-race finish camera & podium lower-third
  kRewardTally,           // Animated Credits / Popularity / Wristband progress tally
  kFinaleIntro,           // Horizon Festival Finale / Darius Flynt intro showdown
  kFinaleVictory,         // Horizon Festival Champion victory celebration
  kCredits,               // Festival Finale Credits Roll (FMV_04 + in-engine credits)
};

const char* campaign_stage_name(CampaignStage stage);
bool parse_campaign_stage(std::string_view name, CampaignStage& out);

// ---- Cast & Character Profiles ----
enum class CharacterId : uint8_t {
  kAliceHart = 0,
  kDakStewart,
  kHollyCruz,
  kScottTyler,
  kDariusFlynt,
  kRamonaCravache,
  kAliHoward,
  kDukeMaguire,
  kMarkoBaran,
  kZakiMalik,
  kHaleyHarper,
  kHaileyHarper = kHaleyHarper,
};

struct CharacterProfile {
  CharacterId id = CharacterId::kAliceHart;
  const char* key = "";
  const char* display_name = "";
  const char* role_tag = "";
  uint32_t accent_rgba = 0xFFF90064u;  // packed 0xAARRGGBB matching ui::rgba
};

const CharacterProfile& character_profile(CharacterId id);
const std::vector<CharacterProfile>& all_characters();

// ---- Canonical FH1 Wristbands, Boss Rivals & Showcases ----
struct WristbandMilestoneSpec {
  int level = 0;  // 0..6 (Yellow..Gold)
  const char* key = "";
  const char* display_name = "";
  const char* colour_key = "";  // "Yellow".."Gold"
  int xp_threshold = 0;
  int64_t nemesis_bonus = 1000;
  const char* vo_unlock_trigger = "";
  const char* vo_new_trigger = "";
  const char* radio_event = "";
  const char* announcement = "";
  int unlocked_events_hint = 3;
};

struct BossRivalSpec {
  int index = 0;          // 0..6
  int wristband_level = 0;
  int64_t event_id = 0;
  const char* horizon_id = "";
  const char* key = "";   // e.g. "ramona", "darius"
  CharacterId character = CharacterId::kRamonaCravache;
  const char* rival_name = "";
  const char* rival_title = "";
  int64_t car_id = 0;
  const char* car_media = "";
  const char* car_display = "";
  int unlock_points = 0;
  int64_t cash_prize = 5000;
  const char* vo_unlocked = "";
  const char* vo_pre_race = "";
  const char* vo_post_win = "";
  const char* challenge_line = "";
  const char* victory_line = "";
  bool is_finale_champion = false;
};

struct ShowcaseMilestoneSpec {
  int index = 0;
  int64_t event_id = 0;
  const char* horizon_id = "";
  const char* display_name = "";
  const char* opponent_label = "";  // e.g. "P-51 MUSTANG", "HOT-AIR BALLOON", "HELICOPTER", "BIPLANE"
  int popularity_rank_req = 250;
  int64_t loaner_car_id = 0;
  const char* loaner_car_media = "";
  const char* loaner_car_display = "";
  int64_t cash_prize = 5000;
  bool airborne = true;
  const char* intro_line = "";
  const char* victory_line = "";
};

const std::vector<WristbandMilestoneSpec>& wristband_milestones();
const WristbandMilestoneSpec* find_wristband_milestone(int level);
const std::vector<BossRivalSpec>& boss_rivals();
const BossRivalSpec* find_boss_by_event(std::string_view horizon_id);
const BossRivalSpec* find_boss_by_key(std::string_view key);
const BossRivalSpec* find_boss_by_level(int wristband_level);
const std::vector<ShowcaseMilestoneSpec>& showcase_milestones();
const ShowcaseMilestoneSpec* find_showcase_by_event(std::string_view horizon_id);

// Optional localized string-table resolver callback (e.g. bound to carview::game_text).
void set_campaign_text_resolver(std::function<std::string(std::string_view)> resolver);

// Resolves a clean human-readable display title for an event, ensuring raw internal
// database keys (e.g. PLANE_RACE_001, FR05, FESTIVAL_SECOND) are never displayed.
std::string resolve_event_display_title(std::string_view raw_id_or_title,
                                        std::string_view localized_override = {});
std::string resolve_event_display_title(const CareerEvent* ev,
                                        std::string_view localized_override = {});

// ---- Cutscene Catalog ----
enum class CutsceneKind : uint8_t {
  kNone = 0,
  kOpeningFmv,
  kWristbandCeremonyFmv,
  kFinaleCreditsFmv,
  kWristbandStageCinematic,
  kShowcaseIntroCinematic,
  kShowcaseOutroCinematic,
  kBossIntroCinematic,
  kBossVictoryCinematic,
  kBarnDiscoveryCinematic,
  kBarnRestoredCinematic,
  kPreRaceGridFlyby,
  kPostRaceFinishCam,
  kFinaleVictoryCinematic,
};

const char* cutscene_id_name(CutsceneKind kind);

// ---- Voiced Character Dialog & Subtitle Queue ----
struct DialogCue {
  std::string id;                   // unique dialog key for heard_dialogs deduplication
  CharacterId speaker = CharacterId::kAliceHart;
  std::string speaker_override;     // optional custom name
  std::string role_override;        // optional custom role tag
  std::string vo_trigger;           // DialogueScript_EN.xml trigger ID
  std::string radio_event;          // RadioSystem.xml DJImmediateDialogue event ID
  std::string subtitle;             // localized subtitle caption
  double duration_s = 5.5;
  int priority = 0;                 // higher priority interrupts lower when `interrupt` is true
  bool one_shot = true;             // if true, skipped once recorded in heard_dialogs
  bool interrupt = false;
};

struct DialogVoTelemetry {
  bool vo_playing = false;
  float vo_elapsed_s = 0.0f;
  float vo_duration_s = 0.0f;
  float vo_rms = 0.0f;
  std::string active_trigger;  // optional lowercase trigger ID for exact clip matching
};

class DialogQueue {
 public:
  bool enqueue(const DialogCue& cue, std::set<std::string>* heard_set = nullptr);
  void set_vo_telemetry(const DialogVoTelemetry& vo);
  void set_vo_telemetry(bool vo_playing, float vo_elapsed_s, float vo_duration_s, float vo_rms,
                        std::string_view active_trigger = {});
  void update(double dt);
  void update(double dt, const DialogVoTelemetry& vo);
  void skip_current();
  void clear();

  bool active() const { return active_; }
  const DialogCue& current() const { return current_; }
  double elapsed() const { return elapsed_; }
  double effective_duration() const { return active_ ? effective_duration_s_ : 0.0; }
  double remaining() const { return active_ ? std::max(0.0, effective_duration_s_ - elapsed_) : 0.0; }
  double progress() const {
    return active_ && effective_duration_s_ > 1e-4 ? std::clamp(elapsed_ / effective_duration_s_, 0.0, 1.0) : 0.0;
  }
  // Smooth [0..1] slide-in/out presentation factor for the call-in banner.
  float banner_slide() const;
  // Smooth [0..1] audio ducking envelope (1.0 = full speech duck on Music/Engine).
  float duck_factor() const { return duck_; }
  float live_vo_rms() const { return vo_rms_; }
  bool vo_hold_active() const { return vo_hold_active_; }
  // Animated audio waveform bar heights [0.1..1.0] driven by live VO RMS when available.
  std::vector<float> waveform_bars(int count) const;

  // Pops newly activated audio triggers (returns true once when a cue starts).
  bool consume_pending_audio(std::string& out_vo_trigger, std::string& out_radio_event);
  size_t queued_count() const { return queue_.size(); }

 private:
  void begin_next();

  bool active_ = false;
  DialogCue current_;
  double elapsed_ = 0.0;
  double effective_duration_s_ = 0.0;
  float duck_ = 0.0f;
  bool audio_pending_ = false;
  bool has_vo_telemetry_ = false;
  bool vo_playing_ = false;
  float vo_elapsed_s_ = 0.0f;
  float vo_duration_s_ = 0.0f;
  float vo_rms_ = 0.0f;
  std::string vo_active_trigger_;
  bool vo_seen_playing_ = false;
  bool vo_hold_active_ = false;
  double vo_ended_elapsed_ = -1.0;
  std::vector<DialogCue> queue_;
};

// ---- UI4 Screen Transition State Machine ----
enum class TransitionKind : uint8_t {
  kNone = 0,
  kBrushWipe,   // signature FH1 hot-pink diagonal brush wipe
  kCrossFade,   // smooth dark/warm cross-fade
  kSlideLeft,   // directional UI4 blade slide
  kSlideUp,     // vertical curtain reveal
};

struct TransitionState {
  TransitionKind kind = TransitionKind::kNone;
  double elapsed_s = 0.0;
  double duration_s = 0.55;
  bool active = false;
  bool midpoint_fired = false;
  std::string label;  // optional transition title strip (e.g. "HORIZON FESTIVAL")

  void start(TransitionKind k, double duration = 0.55, std::string text = {});
  // Returns true on the single frame that crosses the midpoint (t >= 0.5).
  bool update(double dt);
  // [0..1] overall progress.
  double progress() const;
  // [0..1] screen coverage opacity (0 -> 1 at midpoint -> 0 at end).
  float cover_alpha() const;
  // [0..1] brush wipe edge position across screen.
  float wipe_edge() const;
};

// ---- Animated Sequential Post-Race & Milestone Reward Tally ----
enum class PostRaceStage : uint8_t {
  kRaceResults = 0,        // Stage 1: RACE RESULTS podium & standings table
  kCreditsEarned = 1,      // Stage 2: CREDITS EARNED animated breakdown + balance
  kWristbandAndPrize = 2,  // Stage 3: WRISTBAND & POPULARITY + PRIZE CAR AWARDED
};

// Real post-race standing row captured from RaceView::hud_info() / standings (R3-CAM-03).
struct RaceFinishStanding {
  int place = 1;
  std::string driver_name;
  std::string car_name;
  std::string class_name;
  int pi = 0;
  double finish_time_s = -1.0;
  double gap_s = -1.0;
  double best_lap_s = -1.0;
  bool is_player = false;
  bool is_boss = false;
};

struct RewardTallyRow {
  int place = 1;
  std::string driver;
  std::string car;
  std::string car_class;
  int pi = 0;
  double time_s = -1.0;
  double gap_s = -1.0;
  bool player = false;
  double best_lap_s = -1.0;
  bool is_boss = false;
};

struct RewardTallyState {
  bool active = false;
  PostRaceStage stage = PostRaceStage::kCreditsEarned;
  double elapsed_s = 0.0;
  double stage_elapsed_s = 0.0;
  double duration_s = 4.4;
  std::string event_title;
  std::string event_category;
  int place = 1;
  int total_cars = 8;
  bool is_boss = false;
  bool is_showcase = false;
  bool is_finale = false;
  bool qualified = true;
  bool retry_required = false;
  int qualify_cutoff_place = 3;
  std::vector<RewardTallyRow> results_rows;

  // Credit breakdown lines
  int64_t base_credits = 0;
  int64_t mileage_bonus = 0;
  int difficulty_pct = 0;
  int64_t difficulty_bonus = 0;
  int assists_pct = 0;
  int64_t assists_bonus = 0;
  int64_t skills_bonus = 0;
  int64_t nemesis_bonus = 0;
  int64_t total_credits = 0;
  int64_t balance_before = 0;
  int64_t balance_after = 0;

  // Popularity rank climb (lower number = better rank, 250 -> 1)
  int pop_rank_before = 250;
  int pop_rank_after = 250;

  // Wristband points progress bar
  int wb_points_earned = 0;
  int wb_before = 0;
  int wb_after = 0;
  int wb_tier_lo = 0;
  int wb_tier_hi = 220;
  int wb_level = 0;
  std::string wb_current_name = "YELLOW";
  std::string wb_next_name = "GREEN";
  bool wb_tier_up = false;

  // Next unlock toast & prize car
  std::string prize_car_name;
  std::string next_unlock_headline;
  std::string next_unlock_detail;

  // Auto-set GPS prompt for next campaign objective
  bool has_next_gps = false;
  std::string next_gps_label;
  std::array<double, 2> next_gps_xz{};

  void update(double dt);
  void skip_to_end();
  // Advances from current stage to next sequential stage (returns false when already on final stage).
  bool advance_stage();
  double stage_duration_s() const;
  bool stage_animation_done() const { return !active || stage_elapsed_s >= stage_duration_s(); }
  bool is_final_stage() const { return stage == PostRaceStage::kWristbandAndPrize; }
  bool animation_done() const { return !active || (is_final_stage() && stage_animation_done()); }

  int64_t displayed_credits() const;
  int64_t displayed_balance() const;
  int displayed_pop_rank() const;
  int displayed_wb_points() const;
  float wb_bar_fraction() const;
  // [0..1] pulsing flash intensity when crossing a wristband tier threshold.
  float tier_up_flash() const;
  // [0..1] reveal fraction for credit breakdown row `row_idx` (0..5).
  float row_reveal(int row_idx) const;
};

// ---- Active Cinematic Context (read by CampaignDirector & CampaignUI) ----
struct CinematicBanner {
  CutsceneKind kind = CutsceneKind::kNone;
  double elapsed_s = 0.0;
  double duration_s = 6.0;
  bool skippable = true;
  float letterbox_amount = 0.0f;  // 0..1 smooth cinema bar height
  std::string eyebrow;
  std::string headline;
  std::string subheadline;
  std::string detail_line;
  std::string badge_text;
  std::string colour_key;         // e.g. "Yellow".."Gold" for wristband texture
  std::string reward_car;
  uint32_t accent_rgba = 0xFFF90064u;
  int wristband_level = -1;
  int boss_index = -1;
  int showcase_index = -1;
  bool hold_to_skip = false;
  float skip_hold_progress = 0.0f;
};

// ---- Next Campaign Objective (GPS & Free-Roam Tracker) ----
struct CampaignObjective {
  bool valid = false;
  std::string title;
  std::string detail;
  std::string gps_label;
  std::array<double, 2> gps_xz{};
  double distance_m = -1.0;
  bool is_urgent = false;
};

// ---- Commands emitted by CampaignFlow for CampaignDirector / carview ----
struct CampaignAction {
  bool swap_car = false;
  std::string car_media;
  int64_t car_id = 0;
  bool teleport = false;
  std::array<double, 3> pos{};
  double heading = 0.0;
  bool set_gps = false;
  std::string gps_label;
  std::array<double, 2> gps_xz{};
  bool launch_event = false;
  std::string event_id;
  int route_id = 0;
  bool play_fmv = false;
  CutsceneKind fmv_cutscene = CutsceneKind::kNone;
  bool grant_starter = false;
  bool open_autoshow = false;
  bool collect_yellow_wristband = false;
  bool save_requested = false;
  bool intro_completed = false;
  bool enter_free_roam = false;
  bool lock_viper_radio = false;
};

// ---- Serializable Campaign Save Mirror ----
struct CampaignSaveState {
  std::string campaign_stage;
  std::vector<std::string> completed_cutscenes;
  std::vector<std::string> heard_dialogs;
  std::vector<std::string> unlocked_bosses;
  std::vector<std::string> unlocked_showcases;
  bool finale_complete = false;
};

// ---- The campaign mirror of an imported original profile ----
// The original profile stores no campaign mirror (its dialogue record holds
// hashed keys, PROG-15), so an import rebuilds it from the career it does
// store, with the rules CampaignFlow applies live:
// - bosses: every Star Showdown whose rule holds (the collected wristband and
//   points of check_boss_unlocks) or that has a result; Darius once
//   HEADLINE_EVENT is won, with its `headline_event_won` key (the pending
//   showdown) until NEM_FINAL is won, which completes the finale;
// - showcases: every showcase whose popularity gate the rank meets (or, with
//   no rank, that `event_unlocks` holds) or that has a result;
// - heard dialogue: the one-shot unlock and collection lines those imply
//   (boss and showcase unlocks, each collected wristband, the opening's
//   lines), so none is queued again; the opening's cutscenes once the intro
//   is complete, the finale's once it is won.
struct CampaignImportInputs {
  std::vector<std::pair<int64_t, int>> event_results;  // event id, best place
  std::vector<int64_t> event_unlocks;                  // CareerEventUnlocks
  int wristband_points = 0;
  int collected_wristband = 0;  // -1 before the yellow band
  int popularity_rank = 0;      // 1..250; 0 = unknown (use event_unlocks)
  bool intro_completed = true;
};
CampaignSaveState infer_campaign_state(const CampaignImportInputs& in);
// infer_campaign_state over `save`'s career (event results and unlocks,
// wristband points, collected wristband, intro), merged into its mirror:
// the sets are united, the finale kept once set, the stage kept when set.
// `popularity_rank` 0 means unknown (showcases then come from the unlocks).
void seed_imported_campaign(drive::DriveSave& save, int popularity_rank);

// ---- Master Campaign Progression State Machine ----
class CampaignFlow {
 public:
  static constexpr const char* kViperMedia = "VIP_Viper_13";
  static constexpr int64_t kViperCarId = 1544;
  static constexpr const char* kCorradoMedia = "VW_Corrado_95";
  static constexpr int64_t kCorradoCarId = 309;
  static constexpr const char* kQualifyEventId = "FESTIVAL_SECOND";
  static constexpr int kQualifyRouteId = 16;
  static constexpr int kQualifyCutoffPlace = 3;  // Top-3 podium required to qualify in FESTIVAL_SECOND
  static constexpr int kViperConvoyTarget = 5;   // Festival-bound convoy drivers to overtake during Viper drive
  static constexpr double kFestivalArrivalRadiusM = 120.0;
  // Verified Colorado highway asphalt spawn (COLLISION.md measured free-roam spawn).
  static constexpr std::array<double, 3> kViperSpawnPos = {2002.96, 143.38, 960.00};
  static constexpr double kViperSpawnHeading = 2.20073;
  static constexpr std::array<const char*, 2> kViperOpeningPlaylist = {
      "Avicii - Levels (Skrillex Remix)",
      "Porter Robinson - Language",
  };

  // Starts a fresh Minute-0 New Career sequence.
  CampaignAction start_new_career(bool skip_movies,
                                  std::array<double, 3> festival_gate = {-1360.0, 2.0, -210.0});

  // Steps the active stage, dialogs, transitions, cinematics, and reward tally.
  CampaignAction update(double dt, std::array<double, 2> player_xz, bool fmv_active, bool race_finished);

  // Advances or dismisses the active cinematic, ceremony, tally, or onboarding beat.
  CampaignAction advance_or_skip();
  CampaignAction skip_onboarding_all();

  // Event & progression milestone hooks.
  CampaignAction on_enter_event(const CareerEvent& ev);
  CampaignAction on_race_finished(const CareerEvent* ev, const RaceAward& award, int place, int total_cars,
                                  int old_pop_rank, int new_pop_rank, int64_t balance_before,
                                  int64_t balance_after, int wb_points_after, int collected_wristband,
                                  const std::string& prize_car_name, const CareerCatalog* catalog = nullptr,
                                  const std::vector<RaceFinishStanding>& standings = {});
  // Records Festival-bound convoy overtakes during the Viper opening highway drive (R3-CAM-15).
  bool on_viper_convoy_pass(int passed_delta = 1);
  CampaignAction on_wristband_collected(int wristband_level, const std::string& bonus_car_name = {},
                                        int newly_unlocked_events = 0);
  CampaignAction on_barn_find(bool discovered, const std::string& barn_id, const std::string& car_name);
  CampaignAction on_barn_restored(const std::string& barn_id, const std::string& car_name);
  void on_popularity_rank_changed(int old_rank, int new_rank);
  void on_wristband_points_changed(int points, int collected_wristband);
  void start_credits();

  // Deterministic CLI stage jump for `--campaign-stage <stage>` testing & capture.
  // Supports: intro, viper_drive, qualify, registration, freeroam, dialog,
  // wristband_yellow..wristband_gold, showcase, boss_ramona..boss_darius,
  // barn_find, barn_restored, pre_race, post_race, reward_tally, finale, credits.
  bool jump_to_cli_stage(std::string_view cli_stage, CampaignAction& out_action);

  // Save / load persistence.
  void save_to(CampaignSaveState& out) const;
  void load_from(const CampaignSaveState& in);

  // Queries.
  CampaignStage stage() const { return stage_; }
  bool is_onboarding() const;
  bool is_cinematic_active() const;
  bool is_blocking_gameplay() const;
  double stage_time() const { return stage_time_; }
  double distance_to_festival() const { return dist_to_festival_; }
  // Progressive Minute-0 HUD reveal [0..1] during Viper intro drive.
  float hud_reveal_alpha() const;

  const DialogQueue& dialogs() const { return dialogs_; }
  DialogQueue& dialogs() { return dialogs_; }
  const TransitionState& transition() const { return transition_; }
  TransitionState& transition() { return transition_; }
  const CinematicBanner& cinematic() const { return cinematic_; }
  CinematicBanner& cinematic() { return cinematic_; }
  const RewardTallyState& reward_tally() const { return tally_; }
  RewardTallyState& reward_tally() { return tally_; }

  const std::set<std::string>& completed_cutscenes() const { return completed_cutscenes_; }
  const std::set<std::string>& heard_dialogs() const { return heard_dialogs_; }
  const std::set<std::string>& unlocked_bosses() const { return unlocked_bosses_; }
  const std::set<std::string>& unlocked_showcases() const { return unlocked_showcases_; }
  bool finale_complete() const { return finale_complete_; }
  bool pending_darius_showdown() const { return pending_darius_showdown_; }
  bool qualify_retry_pending() const { return qualify_retry_pending_; }
  int viper_convoy_passed() const { return viper_convoy_passed_; }
  int viper_convoy_target() const { return kViperConvoyTarget; }
  int viper_convoy_remaining() const { return std::max(0, kViperConvoyTarget - viper_convoy_passed_); }
  bool starter_autoshow_onboarding() const { return starter_autoshow_onboarding_; }
  bool consume_starter_autoshow_onboarding() {
    const bool v = starter_autoshow_onboarding_;
    starter_autoshow_onboarding_ = false;
    return v;
  }
  int credits_scroll_line_count() const;

  CampaignObjective next_objective(int wristband_points, int collected_wristband, int pop_rank,
                                   const CareerCatalog* catalog = nullptr) const;

 private:
  CampaignAction enter_stage(CampaignStage next, TransitionKind trans = TransitionKind::kNone);
  void mark_cutscene_done(CutsceneKind kind);
  void check_boss_unlocks(int wristband_points, int collected_wristband);
  void check_showcase_unlocks(int pop_rank);

  CampaignStage stage_ = CampaignStage::kTitle;
  double stage_time_ = 0.0;
  bool skip_movies_ = false;
  bool finale_complete_ = false;
  bool qualify_launched_ = false;
  bool qualify_retry_pending_ = false;
  int viper_beat_idx_ = 0;
  int viper_convoy_passed_ = 0;
  int reg_beat_idx_ = 0;
  int active_wristband_ = 0;
  int active_boss_idx_ = -1;
  int active_showcase_idx_ = -1;
  bool pending_finale_credits_ = false;
  bool pending_darius_showdown_ = false;
  bool starter_autoshow_onboarding_ = false;

  std::array<double, 3> viper_spawn_ = kViperSpawnPos;
  double viper_heading_ = kViperSpawnHeading;
  std::array<double, 3> festival_gate_ = {-1360.0, 2.0, -210.0};
  double dist_to_festival_ = 3560.0;

  DialogQueue dialogs_;
  TransitionState transition_;
  CinematicBanner cinematic_;
  RewardTallyState tally_;

  std::set<std::string> completed_cutscenes_;
  std::set<std::string> heard_dialogs_;
  std::set<std::string> unlocked_bosses_;
  std::set<std::string> unlocked_showcases_;
};

}  // namespace race
