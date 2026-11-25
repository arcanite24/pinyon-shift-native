// Application-lifetime state for `carview --drive` (persists across world/car
// reloads): SDL window, saves, garage, career progression, audio host, ToD,
// collectibles, outposts, and Horizon Rally state.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "audio_host.h"
#include "campaign_director.h"
#include "career_page.h"
#include "cars/physics.h"
#include "cars/sources.h"
#include "drive/save.h"
#include "drive_cli.h"
#include "drive_helpers_runtime.h"
#include "fmv_player.h"
#include "freeroam/club.h"
#include "freeroam/collectibles.h"
#include "freeroam/outposts.h"
#include "freeroam/road_discovery.h"
#include "freeroam/skills.h"
#include "freeroam/speed_zones.h"
#include "game_hud.h"
#include "garage/garage.h"
#include "garage/pi_calc.h"
#include "map_view.h"
#include "mods_page.h"
#include "playtest_recorder.h"
#include "race/career.h"
#include "race/rally_championship.h"
#include "race_view.h"
#include "rally_hub.h"
#include "reference.h"
#include "save_sync_ui.h"
#include "story_intro.h"
#include "traffic/gps.h"
#include "traffic/nav.h"
#include "world/smashables.h"
#include "world/tod.h"
#include "world/vfx.h"
#include "world/world_anim.h"

namespace carview {

struct DriveApp : DriveConfig {
  struct PendingCampaignHandoff {
    bool active = false;
    bool has_teleport = false;
    std::array<double, 3> pos{};
    double heading = 0.0;
    bool has_gps = false;
    std::string gps_label;
    std::array<double, 2> gps_xz{};
    bool has_event = false;
    std::string event_id;
    int route_id = 0;
    bool open_autoshow = false;
    bool lock_viper_radio = false;
  };

  struct PendingBarnRestore {
    std::string barn_id;
    std::string car_name;
    int64_t car_id = 0;
    double ready_after_s = 45.0;
    double elapsed_s = 0.0;
    bool ready_to_claim = false;
    bool notified_ready = false;
  };

  struct RivalsAttempt {
    bool active = false;
    bool is_rally_stage = false;
    std::string event_id;
    std::string rival_name;
    double target_time_s = 0.0;
    int64_t bounty_cr = 0;
    int tier_index = 0;  // 0 = Personal Best, 1..3 = Bronze/Silver/Gold target ghost
    bool completed = false;
  };

  struct Logo {
    std::vector<uint8_t> rgba;
    uint32_t width = 0, height = 0;
  };

  UiFontCache font_cache;
  bool again = false;
  bool car_given = false;
  bool slice12_modded_save = false;
  std::filesystem::path clean_user_dir;
  ModsSession mods_session;
  SaveSession save_session;
  drive::DriveSave saved;
  bool have_saved = false;
  std::filesystem::path state_root;
  std::shared_future<std::shared_ptr<cars::PartsDb>> parts_db;
  std::shared_ptr<race::CarNames> car_names = std::make_shared<race::CarNames>();
  std::shared_ptr<bool> car_names_loaded = std::make_shared<bool>(false);

  garage::Garage career_garage;
  bool have_garage = false;
  std::filesystem::path garage_path;
  std::map<int64_t, std::string> baked_ids;
  bool imported = false;
  std::vector<garage::ClassBand> class_bands;
  const cars::PartsDb* shared_db = nullptr;

  race::CareerCatalog career_catalog;
  bool have_career_catalog = false;
  freeroam::SkillsData skills_data;
  bool have_skills_data = false;

  bool show_title = false;
  FmvPlayer fmv_player;
  FmvPlayer title_video;
  bool title_video_tried = false;
  StoryIntro story_intro;
  CampaignDirector campaign;
  bool campaign_new_career_pending = false;
  PendingCampaignHandoff pending_handoff;
  std::vector<PendingBarnRestore> pending_barn_restorations;
  bool barn_restorations_reconciled = false;
  bool pending_autoshow_open = false;
  bool campaign_owns_post_race = false;
  bool boot_intro_checked = false;
  bool title_menu_phase = false;
  bool title_confirm_new = false;
  int title_selected = 0, title_confirm_selected = 1;
  bool new_career_save_pending = false;
  std::string pre_loaner_car;
  int64_t pre_loaner_garage_id = 0;
  std::string pending_event_loaner_car;
  bool pr_stunt_loaner_active = false;

  std::unique_ptr<RaceView> race;
  race::CareerRecord career;
  race::WristbandTables wristbands;
  double race_top_speed = 0;
  cars::PhysicsSettings race_phys_settings;
  garage::PiConfig race_pi_cfg = garage::synthetic_pi_config();
  bool race_pi_cfg_loaded = false;

  int dynamic_ai_skill = 10;
  static int dynamic_skill_for_difficulty(race::Difficulty d) {
    switch (d) {
      case race::Difficulty::kEasy: return 18;
      case race::Difficulty::kMed: return 10;
      case race::Difficulty::kHard: return 3;
      case race::Difficulty::kPro:
      case race::Difficulty::kHardPlus: return 1;
    }
    return 10;
  }
  race::ProgressionTables prog_tables;
  bool have_prog_tables = false;
  bool legacy_fm4_progression = false;  // R3-CAM-14: OFF by default for 1:1 FH1 economy
  RivalsAttempt active_rivals;          // R3-CAM-07: active Rivals / Time Trials ghost challenge
  int h2h_races = 0;
  int h2h_wins = 0;
  freeroam::SpeedActivitiesData speed_data;
  std::unique_ptr<freeroam::SpeedActivityTracker> speed_tracker;
  int collected_wristband = 0;
  // Street race hubs' unlock state as last seen, for their RadioDJRequests lines.
  std::map<int, bool> street_hub_radio_unlocked;
  double street_hub_radio_check_at = -1e9;
  std::unique_ptr<freeroam::SkillTracker> skill_tracker;
  int64_t max_skills_car = 0;
  std::string sponsor_banner_title, sponsor_banner_detail;
  double sponsor_banner_at = -1e9;

  bool title_saved_resume_pending = false;
  double saved_resume_spawn[4] = {0, 0, 0, 0};
  bool front = false;
  bool cruise = false;
  WorldReference world_reference;
  std::string error;
  std::vector<drive::ScriptLine> script;

  SDL_Window* window = nullptr;
  int status = 0;

  std::vector<std::string> baked_cars;
  std::vector<int64_t> pick_ids;
  std::set<std::string> all_baked;
  std::function<void()> sync_picker;
  std::string next_car;
  std::vector<std::string> event_ids;
  std::map<std::string, std::string> event_names;
  size_t event_pick = 0;
  std::string next_event;
  std::string next_event_fallback;
  bool leave_race = false;

  freeroam::Collectibles collectibles;
  freeroam::Progress progress;
  bool progress_loaded = false;
  MapView map_view;
  bool map_tried = false;
  traffic::NavMap shared_nav;
  bool shared_nav_loaded = false;
  traffic::GpsSolver gps_solver;
  traffic::GpsNavigator gps_nav{&gps_solver};
  freeroam::RoadDiscovery road_discovery;
  freeroam::OutpostCatalog outposts;
  freeroam::OutpostProgress outpost_progress;
  freeroam::PrStuntSession pr_stunt_session;
  bool have_outposts = false;
  bool outposts_tried = false;
  bool cli_gps_applied = false;
  bool cli_pr_stunt_applied = false;

  AudioHost audio_host;
  bool radio_logos_tried = false;
  std::filesystem::path user_music_dir;
  std::map<std::string, Logo> radio_logos;

  int64_t paint_sequence = -1;
  std::string paint_car;

  world::TodSystem tod_system;
  world::TodClock tod_clock;
  double pre_race_tod_sec = -1.0;
  world::ParticleLibrary vfx_lib;
  world::SmashableWorld smashables;
  world::AnimatedWorldSystem world_anim;
  std::future<bool> fut_tod_;
  std::future<bool> fut_vfx_;
  std::future<bool> fut_smash_;
  std::future<bool> fut_anim_;
  bool anim_shared_bound_ = false;

  freeroam::club::ClubData club_data;
  bool club_tried = false, club_loaded = false;
  std::string club_why;

  std::filesystem::path colorado_world_dir;
  bool has_colorado_return = false;
  std::array<double, 3> colorado_return_pos{};
  double colorado_return_heading = 0.0;
  race::RallyCatalog rally_catalog;
  bool rally_catalog_tried = false;
  race::RallyChampionshipSession rally_session;
  bool rally_hub_open = false;
  int rally_hub_selected = 0;
  bool rally_service_open = false;
  bool rally_service_return = false;
  bool rally_service_resume_after_reload = false;
  int rally_service_selected = 0;
  bool rally_service_repaired = false;
  bool rally_results_open = false;
  RallyChampResultsState rally_results_state{};
  int rally_world_switch = 0;
  std::string rally_notice;

  bool autoshow_buy_confirm = false;
  bool hub_orbit_dragging = false;
  bool hub_orbit_dragged = false;
  float hub_orbit_last_x = 0.0f;
  float hub_orbit_last_y = 0.0f;

  int pose_shot_counter = 0;
  PlaytestRecorder playtest_recorder;

  bool init(int& exit_code);
  bool colorado_world() const;
  std::filesystem::path game_root_of() const;
  void write_career_garage();
  bool start_race(const std::string& id, std::string& why);
  void restore_pre_race_tod();
  int current_pop_rank() const;
  std::vector<std::string> class_name_list() const;
  std::string award_garage_car(int64_t car_id, int colour_seq, bool select_it, bool is_prize = false);
  CareerSummary career_summary(std::vector<cars::PackInfo>& packs);
  CareerBoard career_board(const CareerPage& page);
  size_t garage_picker();
  void reload_world_assets(bool init_anim);
  void start_world_assets_async(bool init_anim);
  void wait_tod_ready();
  void wait_vfx_ready();
  void wait_smash_ready();
  void wait_anim_ready();
  void wait_all_world_assets();
  bool handle_rally_world_switch();
  void shutdown();
};

}  // namespace carview
