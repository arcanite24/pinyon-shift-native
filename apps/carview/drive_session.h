// Per-car / per-race interactive or headless session for `carview --drive`.
// Constructed fresh on every hot-reload iteration while DriveWorldHost keeps
// Vulkan pipelines, WorldGpu, World, Collision, and Streamer resident.
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>

#include "achievements.h"
#include "audio/audio_engine.h"
#include "autoshow.h"
#include "career_page.h"
#include "cars/physics.h"
#include "club_view.h"
#include "contact_log.h"
#include "drive/cameras.h"
#include "drive/controls.h"
#include "drive/damage.h"
#include "drive/driving_line.h"
#include "drive/lamps.h"
#include "drive/replay.h"
#include "drive/rewind.h"
#include "drive/rumble.h"
#include "drive/vehicle.h"
#include "drive_app.h"
#include "drive_helpers.h"
#include "drive_helpers_runtime.h"
#include "drive_world_host.h"
#include "events_page.h"
#include "fh1data/spa.h"
#include "freeroam/club.h"
#include "freeroam/festival_ai.h"
#include "game_hud.h"
#include "garage/pi_calc.h"
#include "hub_scene.h"
#include "hud.h"
#include "livery_editor.h"
#include "map_view.h"
#include "mods_page.h"
#include "photo_mode.h"
#include "platform_support.h"
#include "race/campaign_flow.h"
#include "race_view.h"
#include "replay_view.h"
#include "safe_reset.h"
#include "save_sync_ui.h"
#include "settings_page.h"
#include "slice12_runtime.h"
#include "story_intro.h"
#include "traffic/nav.h"
#include "traffic_view.h"
#include "ui.h"
#include "upgrade_shop.h"
#include "world/smashables.h"
#include "world/tod.h"

namespace carview {

struct DriveSession {
  static constexpr double kIntroSeconds = 6.0;

  struct RaceModelsGuard {
    RaceView* r = nullptr;
    ~RaceModelsGuard() {
      if (r) r->release_models();
    }
  };

  struct FestivalGuestsGuard {
    Showroom* host_showroom = nullptr;
    std::map<std::string, std::unique_ptr<Showroom>>* map = nullptr;
    ~FestivalGuestsGuard() {
      if (host_showroom) host_showroom->set_instances({});
      if (map) map->clear();
    }
  };

  struct ContactLog {
    FILE* f = nullptr;
    DrawnIndex drawn, all;
    int gathers = -1;
    bool touching = false;
    drive::Vec3 last{};
    size_t contacts = 0, undrawn = 0;
    ~ContactLog() {
      if (!f) return;
      std::fprintf(f, "summary: %zu hard contacts, %zu with nothing drawn within 1 m\n", contacts, undrawn);
      std::printf("contacts: %zu hard, %zu with nothing drawn within 1 m\n", contacts, undrawn);
      std::fclose(f);
    }
  };

  struct Popup {
    std::string title, text;
    double at = -1e9, progress = -1;
    uint32_t colour = 0;
  };

  struct ClubRun {
    size_t index = 0;
    freeroam::club::Run run;
    int failures_seen = 0;
  };

  struct ClubPendingPulses {
    std::vector<freeroam::club::SkillAward> skills;
    bool skill_chain_completed = false;
    std::vector<std::string> smashables;
    std::vector<freeroam::club::SpeedCameraHit> cameras;
    bool event_started = false;
    bool event_restarted = false;
    bool event_completed = false;
    int event_finish_place = 0;
    bool encounter_started = false;
    bool encounter_completed = false;
    int encounter_finish_place = 0;
    void clear() {
      skills.clear();
      skill_chain_completed = false;
      smashables.clear();
      cameras.clear();
      event_started = event_restarted = event_completed = false;
      event_finish_place = 0;
      encounter_started = encounter_completed = false;
      encounter_finish_place = 0;
    }
  };

  enum class PendingShot { kNone, kF8, kF12, kPhotoMode };

  struct WorldRewindSnapshot {
    bool has_race = false;
    RaceView::RewindSnapshot race;
    bool has_traffic = false;
    TrafficView::RewindSnapshot traffic;
    bool has_festival = false;
    freeroam::FestivalManager::RewindSnapshot festival;
    bool has_skills = false;
    freeroam::SkillTracker::RewindSnapshot skills;
    world::SmashableWorld::RewindSnapshot smashables;
  };

  struct OutpostHubItem {
    enum Kind { kPrStunt, kGarage, kNearbyEvents, kFastTravelFestival, kFastTravelOutpost } kind = kPrStunt;
    std::string label;
    std::string ref_id;
    std::string detail_title;
    std::vector<std::string> detail_lines;
    double dest_x = 0.0, dest_z = 0.0;
  };

  DriveApp& app;
  DriveWorldHost& host;
  const cars::bake::CarInfo& info;

  bool hot_reload_car = false;
  std::vector<std::string> physics_notes;
  audio::UpgradeLevels audio_upgrades{};
  int audio_trans_level = 0, audio_turbo_level = 0, audio_sc_level = 0;
  drive::Vehicle vehicle;

  std::vector<float> audio_track;
  std::unique_ptr<TrafficView> traffic;
  RaceModelsGuard race_models;

  traffic::NavMap festival_nav;
  traffic::Roads festival_roads;
  std::unique_ptr<freeroam::FestivalManager> festival_mgr;
  std::map<std::string, std::unique_ptr<Showroom>> festival_guests;
  FestivalGuestsGuard festival_guard;

  SafeReset safe_reset;
  bool safe_reset_ok = false;
  ContactLog contact_log;

  drive::Vec3 last_finite{};
  double last_finite_kmh = 0;
  std::string notice;
  double notice_until = -1;
  Popup popup;
  bool save_due = false;
  bool skip_end_save = false;
  bool ui_atlas_synced = false;

  std::vector<ClubRun> club_runs;
  int64_t club_car = -1;
  std::string club_gate;
  bool club_open = false;
  size_t club_pick = 0;

  SettingsPage settings_page;
  bool settings_live_overlay = false;
  Slice12Runtime slice12;
  LiveSettings boot_live_settings{};
  ModsPage mods_page;
  SaveProfilePage save_page;

  freeroam::club::Signal club_signal;
  ClubPendingPulses club_pending;
  ClubMedalCeremony club_ceremony{};
  double club_ceremony_at = -1e9;
  bool club_race_was_started = false;
  bool club_race_was_finished = false;
  bool club_was_air = false;
  drive::Vec3 club_takeoff{};
  std::map<int64_t, std::string> club_event_names;
  std::map<int64_t, freeroam::club::CarMeta> club_car_meta;
  bool club_meta_built = false;

  bool pr_stunt_photo_pressed = false;
  bool pr_stunt_has_prev = false;
  double pr_stunt_prev_x = 0.0, pr_stunt_prev_z = 0.0;

  int prev_race_place = 0;
  std::string won_car_name;
  bool have_touch = false;
  double last_touch_y = 0;
  bool have_prev_speed_pos = false;
  double prev_speed_x = 0.0, prev_speed_z = 0.0;
  bool demo_h2h_started = false, demo_speed_trap_done = false;
  RigAccelState rig_accel;
  double step_physics_ms = 0.0;
  double step_ai_traffic_ms = 0.0;
  double step_world_stream_ms = 0.0;
  double step_object_impact_accum = 0.0;
  double step_car_impact_accum = 0.0;

  int last_lut_zone = -2;
  float last_lut_day = -1.0f, last_lut_night = -1.0f;
  double tod_frame_dt = 0.0;
  bool tod_cockpit_cam = false;
  bool live_motion_blur = true;
  bool live_dof = true;
  bool live_photo_through_walls = false;
  bool live_photo_wider_limits = false;
  PhotoModeController photo_mode;
  HorizonPromoTracker promo_tracker;
  std::filesystem::path photos_dir;
  std::filesystem::path replays_dir;
  bool ach_isolated = false;
  std::filesystem::path achievements_path;
  fh1::SpaDatabase spa_db;
  AchievementsSystem achievements;
  double next_achievement_eval = 0.0;
  drive::ReplayRecorder replay_rec;
  ReplayViewer replay_viewer;
  int replay_last_lap = 1;
  bool replay_finish_marked = false;
  PendingShot pending_shot = PendingShot::kNone;
  std::string pending_f12_pose_name;

  world::PostZoneBlender zone_blender;
  drive::CarLampState player_lamps;
  drive::CockpitAnimState cockpit_anim;
  float cockpit_steer_kick_rad = 0.0f;
  std::array<double, 4> cam_prev_wheel_comp{};
  double cam_car_coll_long = 0.0, cam_car_coll_lat = 0.0;
  float cockpit_dial_max_speed = 80.0f;
  float cockpit_dial_max_boost = 15.0f;
  bool cockpit_has_fi = false;
  int light_override = 0;

  drive::ControlSettings live_controls;
  drive::CarDamageState car_damage;
  bool live_sound_enabled = true;
  bool live_radio_enabled = true;
  bool loaded_camera_effects = true;

  WheelPose poses[4]{};
  CameraPull camera_pull;
  drive::DriveCamera rig;
  bool have_rig = false;
  drive::GamepadRumble gamepad_rumble;
  bool rumble_enabled = true;
  double rumble_scale = 1.0;
  int prev_rumble_gear = 1;
  uint32_t prev_rumble_backfire = 0;
  double rig_min_distance = 1e9, rig_max_distance = 0;

  drive::RewindBuffer rewind_buf;
  std::unordered_map<uint64_t, WorldRewindSnapshot> world_rewind_snaps;
  uint64_t last_rewind_smash_snap = 0u;
  drive::BrakingTable braking_table;
  drive::DrivingLineMode driving_line_mode = drive::DrivingLineMode::kFull;
  int pending_shift = 0;
  std::vector<drive::DrivingLineSample> dl_samples;
  std::vector<drive::DrivingLineVertex> dl_verts;
  std::vector<WorldVertex> overlay_verts;
  drive::CameraView last_cam_view{};
  bool have_cam_view = false;

  std::string intro;
  int hud_height = 0;
  std::vector<ui::HitZone> last_ui_hits;
  int last_ui_w = 1280, last_ui_h = 720;

  UpgradeShop shop;
  bool paint_open = false;
  LiveryEditor livery_editor;
  std::filesystem::path livery_edit_path;
  std::vector<size_t> paint_order;
  size_t paint_pick = 0, paint_before = 0;
  Autoshow autoshow;
  GarageBrowser garage_browser;
  int64_t garage_sell_confirm_id = -1;
  BarnFindCard barn_card;
  cars::PhysicsSettings hub_phys_settings;
  garage::PiConfig hub_pi_cfg = garage::synthetic_pi_config();
  CareerPage career_page;
  EventsPage events_page;
  CeremonyHud ceremony;
  bool garage_open = false;
  size_t garage_pick = 0;
  bool grid_menu_open = false;
  double pre_race_blend_t = -1.0;
  int grid_menu_selected = 0;
  double title_orbit_time = 0.0;
  WorldTrigger active_trigger;
  WorldMarkerHud world_marker;
  std::vector<NameTagHud> name_tags;
  MapOverlayHud map_overlay;
  std::vector<MapMarker> cached_map_markers;
  int map_selected_marker = -1;
  bool legacy_hud_cleared = false;
  int results_page = 0;
  bool results_table = false;
  bool results_points = false;
  std::string pending_event_launch;
  bool handoff_applied = false;
  std::vector<race::CampaignAction> queued_campaign_actions;

  std::array<float, 4> mm_uv{};
  float mm_x = 0, mm_y = 0, mm_size = 0;
  double points_at = -1;
  bool outpost_hub_open = false;
  std::string outpost_hub_id;
  size_t outpost_hub_pick = 0;
  std::vector<OutpostHubItem> outpost_hub_items;
  RadioPopupGpuState radio_popup_gpu;
  double minimap_at = -1;

  // Dial needle trail EMA (replaces function-local static in game_hud.cpp)
  double dial_trail_last_rpm = 0.0;
  double dial_trail_last_t = -1.0;
  double dial_trail_rate = 0.0;

  // Interactive window loop state
  std::vector<SDL_Gamepad*> pads;
  uint64_t last_tick_ns = 0, title_at = 0;
  double accumulator = 0;
  bool was_sim_paused = false;
  drive::Vehicle::Pose drawn_before{};
  bool running = true;
  size_t pick = 0;
  bool picking = false;
  bool show_hud = true;
  PauseMenu menu;
  double next_save = 30;
  bool map_open = false, map_dirty = false, map_dragging = false;
  double map_zoom = 2, map_cx = 0, map_cz = 0;
  MapFilter map_filter = MapFilter::kAll;
  MapCameraController map_cam;
  int map_w = 0, map_h = 0;
  bool window_minimized = false;
  bool window_occluded = false;
  AppLifecycleState app_lifecycle;

  // Replaces function-local statics in main window loop
  uint32_t mod_frame_counter = 0;
  bool trainer_freecam_was_active = false;

  DriveSession(DriveApp& a, DriveWorldHost& h);
  void init_session();
  void run(bool& session_again);
  void finalize_session(bool& session_again);

  // Diagnostics & persistence (drive_session.cpp)
  void census_frame(uint32_t width, uint32_t height);
  std::string drawn_name(uint32_t t);
  void log_contacts();
  void reload_mod_textures();
  void save_now(const char* why);
  void eval_achievements(bool toasts);

  // 1000 Club (drive_club.cpp)
  void club_ensure_meta();
  freeroam::club::ClubMedalStats club_medal_stats();
  drive::SavedClubCar* club_record();
  void club_check();
  void club_step();
  ClubScreenState build_club_screen();
  void club_page(std::string& title, std::vector<std::string>& lines);
  ClubActiveStrip build_club_active_strip();
  std::string club_banner();

  // Simulation, audio & world streaming (drive_simulation.cpp)
  audio::EnginePlayer::Update sound_inputs(audio::View audio_view);
  void fire_satnav_event(const traffic::GpsPromptEvent& gev);
  void start_pr_stunt(const freeroam::PrStunt& st);
  bool free_roam_step();
  int64_t current_make_id();
  bool step(const drive::Input& user_in);
  void open_replay_viewer(bool browser_if_empty);
  std::vector<PromoCarCandidate> gather_promo_candidates();
  void place_world();
  audio::View audio_view();
  void prune_world_rewind();
  void capture_world_rewind(uint64_t snap_id);
  void apply_world_rewind_scrub();
  void restore_world_rewind(uint64_t snap_id, bool is_resume);

  // HUD & minimap sync (drive_hud_sync.cpp)
  ShowroomFrame drive_frame(float aspect, double dt);
  std::string race_banner();
  int affinity_discount_pct();
  int discount();
  void current_car_info(int64_t& out_car_id, int& out_class_id);
  void rebuild_outpost_hub();
  void open_outpost_hub(const std::string& id);
  void set_hud(int win_w, int win_h, const std::string& banner, const PauseMenu* menu = nullptr);
  void append_modern_overlays(ui::List& target_list);
  void set_radio_popup(int height);
  void radio_press(int dir);
  MapScene map_scene();
  void set_minimap(int height, bool force);

  // Menus & world triggers (drive_menus_dispatch.cpp)
  void sync_pick();
  void browse(int by);
  void take_pick();
  void put_back();
  void open_garage_browser();
  void sync_livery_preview(bool recomposite_vinyls);
  void open_paint();
  void open_shop_or_autoshow(bool is_autoshow);
  bool collect_next_wristband();
  void open_events_browser(int hub_filter);
  bool try_enter_event(const race::CareerEvent& ev);
  void update_driving_line_overlay();
  void start_new_career();
  bool map_marker_visible(const MapMarker& m);
  void update_map_selection();
  void open_map();
  void close_map();
  void sync_pause_labels();
  void cycle_difficulty(int by);
  void choose();
  void race_continue();
  void race_restart();
  void open_menu();
  bool activate_world_trigger();

  // Headless / world-reference / benchmark runners (drive_benchmark_runner.cpp)
  void print_scenery_summary();
  void run_world_reference();
  void run_benchmark();
  bool run_demo_captures();
  void run_headless_drive();

  // Windowed SDL event & input handlers (drive_input_*.cpp)
  bool ui_mouse_hit(float mx, float my, ui::HitZone& out);
  bool handle_campaign_and_title_event(const SDL_Event& e);
  bool handle_grid_and_map_event(const SDL_Event& e);
  bool handle_events_and_outpost_event(const SDL_Event& e);
  bool handle_hub_shop_event(const SDL_Event& e);
  bool handle_rally_career_club_event(const SDL_Event& e);
  void persist_slice12_settings();
  bool handle_dev_overlays_event(const SDL_Event& e);
  bool handle_settings_and_media_event(const SDL_Event& e);
  void try_trigger_rewind();
  void poll_sdl_events();
  void sample_driving_input(drive::Input& in, double& rig_look);

  // Interactive window loop (drive_render_loop.cpp)
  void apply_story_action(const StoryAction& sa);
  std::string find_baked_car(const std::string& media, int64_t car_id);
  void apply_campaign_action(const race::CampaignAction& ca);
  bool render_window_frame(uint64_t now, double frame_dt, bool menu_paused, bool trainer_freecam_active);
  void run_window_loop();
};

}  // namespace carview
