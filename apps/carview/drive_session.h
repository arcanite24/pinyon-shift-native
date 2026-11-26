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
#include "club_menu.h"
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
#include "hud_notify.h"
#include "events_page.h"
#include "fh1data/spa.h"
#include "freeroam/club.h"
#include "freeroam/festival_ai.h"
#include "game_hud.h"
#include "garage/pi_calc.h"
#include "car_list_screen.h"
#include "options_screen.h"
#include "stats_screen.h"
#include "hub_menus.h"
#include "hub_scene.h"
#include "hud.h"
#include "livery_editor.h"
#include "map_cursor.h"
#include "map_screen.h"
#include "map_view.h"
#include "mods_page.h"
#include "photo_mode.h"
#include "platform_support.h"
#include "race/campaign_flow.h"
#include "race/reset_rules.h"
#include "race_screens.h"
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
    Renderer* renderer = nullptr;
    Showroom* host_showroom = nullptr;
    std::map<std::string, std::unique_ptr<Showroom>>* map = nullptr;
    std::map<std::string, std::future<std::unique_ptr<Showroom>>>* pending = nullptr;
    ~FestivalGuestsGuard() {
      if (host_showroom) host_showroom->set_instances({});
      std::vector<std::unique_ptr<Showroom>> drained;
      if (pending) {
        for (auto& [k, fut] : *pending) {
          if (fut.valid()) {
            if (auto sr = fut.get()) drained.push_back(std::move(sr));
          }
        }
        pending->clear();
      }
      if (renderer && ((map && !map->empty()) || !drained.empty())) renderer->wait_idle();
      for (auto& sr : drained)
        if (sr) sr->skip_wait_idle_on_destroy();
      if (map) {
        for (auto& [k, sr] : *map)
          if (sr) sr->skip_wait_idle_on_destroy();
        map->clear();
      }
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
    // The original notification it is (its lines from the string tables),
    // its icon, the tag over it and a sponsor's logo texture.
    NotifyKind kind = NotifyKind::kEngine;
    NotifyIcon icon = NotifyIcon::kMisc;
    std::string tag, logo;
    // The original notification `kind`, its lines formatted from the string
    // tables ("{0}".. from the arguments).
    static Popup notify(NotifyKind kind, double at, const std::vector<std::string>& title_args = {},
                        const std::vector<std::string>& message_args = {},
                        const std::vector<std::string>& tag_args = {});
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

  enum class PendingShot { kNone, kF8, kF12, kPhotoMode, kHubShot };

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
    drive::CarDamageState car_damage;
  };

  struct OutpostHubItem {
    enum Kind { kPrStunt, kGarage, kNearbyEvents, kFastTravelFestival, kFastTravelOutpost } kind = kPrStunt;
    std::string label;
    std::string ref_id;
    std::string detail_title;
    std::vector<std::string> detail_lines;
    double dest_x = 0.0, dest_z = 0.0;
  };

  // Reset-to-Road UX state machine, timed by GameTunableSettings.ini
  // [HandOfGodReset] (reset_timing): FadeOutTime fade-out -> safe
  // road-centerline reposition with zero velocity & camera re-center ->
  // FadeInTime fade-in. Car contact is off for CollisionsOffTime from the
  // start of the fade-out and for PostResetNonCollideTime after the
  // reposition (the split between the two timers is inferred from their
  // names, not traced).
  struct ResetToRoadState {
    enum class Phase : uint8_t { kIdle = 0, kFadeOut, kFadeIn };
    Phase phase = Phase::kIdle;
    double elapsed_s = 0.0;
    double ghost_remaining_s = 0.0;
    drive::Vehicle::Pose target_pose{};
    bool has_target_pose = false;
    drive::Vehicle::Pose last_live_pose{};
    bool has_last_live_pose = false;
    uint64_t seen_rig_restarts = 0;
    std::string pending_notice;

    bool active() const { return phase != Phase::kIdle; }
    // No car contact: the post-reset window, or the first `collisions_off`
    // s of the fade-out.
    bool ghost_active(double collisions_off) const {
      return ghost_remaining_s > 0.0 || (phase == Phase::kFadeOut && elapsed_s < collisions_off);
    }
  };
  race::HandOfGodReset reset_timing;  // loaded with the session (fallbacks: the file's values)

  DriveApp& app;
  DriveWorldHost& host;
  const cars::bake::CarInfo& info;

  bool hot_reload_car = false;
  std::vector<std::string> physics_notes;
  audio::UpgradeLevels audio_upgrades{};
  int audio_trans_level = 0, audio_turbo_level = 0, audio_sc_level = 0;
  bool audio_sc_is_centrifugal = false;
  int audio_camshaft_level = 0;
  int prev_audio_checkpoint = 0;
  int prev_audio_lap = 0;
  int prev_audio_skill_mult = 1;
  uint32_t prev_codriver_completed = 0;
  drive::Vehicle vehicle;

  std::vector<float> audio_track;
  std::unique_ptr<TrafficView> traffic;
  RaceModelsGuard race_models;

  traffic::NavMap festival_nav;
  traffic::Roads festival_roads;
  std::unique_ptr<freeroam::FestivalManager> festival_mgr;
  std::map<std::string, std::unique_ptr<Showroom>> festival_guests;
  std::map<std::string, std::future<std::unique_ptr<Showroom>>> festival_guest_loads;
  FestivalGuestsGuard festival_guard;

  SafeReset safe_reset;
  bool safe_reset_ok = false;
  ResetToRoadState reset_ux;
  ContactLog contact_log;

  drive::Vec3 last_finite{};
  double last_finite_kmh = 0;
  std::string notice;
  double notice_until = -1;
  Popup popup;
  // The wristband last announced as ready to collect (-1: none).
  int notified_wristband = -1;
  bool save_due = false;
  bool skip_end_save = false;
  bool ui_atlas_synced = false;
  bool pending_title_resume_gather = false;

  std::vector<ClubRun> club_runs;
  int64_t club_car = -1;
  std::string club_gate;
  // Why club_gate is set: the menu's gate (kSystem: not loaded, not in
  // Colorado or the package off; kWristband: no green wristband) or the
  // driven car's (kCar: not a garage car; kNoChallenges).
  enum class ClubGate : uint8_t { kNone, kSystem, kWristband, kCar, kNoChallenges } club_gate_kind = ClubGate::kNone;
  bool club_open = false;
  size_t club_pick = 0;
  // The 1000 Club's menus on their scenes (club_menu.h); club_open with
  // club_menu closed draws the hand-drawn screen (no v4 scenes).
  ClubMenu club_menu;
  ClubMenuData club_menu_data;
  // A HUD item's pass or fail (shown for kClubResultHold).
  struct ClubResult {
    size_t index = 0;
    bool passed = false;
    double at = 0.0;
  };
  std::vector<ClubResult> club_results;
  // The v4 HUD drew the challenges last frame: no bottom banner over its
  // prompt.
  bool club_v4_hud_shown = false;
  // Horizon Rally's entry (V4-10): the gate or confirmation popup over the
  // world, then the first entry's video before the switch.
  RallyEntryPopup rally_entry = RallyEntryPopup::kNone;
  bool rally_enter_after_video = false;
  void rally_entry_press(bool accept);
  void rally_enter();
  // The pause rows of the title update's packages (sync_pause_labels).
  bool pause_packs_read = false, pause_has_club = false, pause_has_rally = false;
  bool car_club_open = false;
  int car_club_pick = 0;
  std::string car_club_notice;

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
  bool demo_h2h_started = false;
  int demo_speed_trap_passes = 0;
  RigAccelState rig_accel;
  double step_physics_ms = 0.0;
  double step_ai_traffic_ms = 0.0;
  double step_world_stream_ms = 0.0;
  double step_object_impact_accum = 0.0;
  double step_car_impact_accum = 0.0;

  int last_lut_zone = -2;
  float last_lut_day = -1.0f, last_lut_night = -1.0f;
  world::ColorLut16 cur_post_lut{};
  world::ColorLut16 target_post_lut{};
  bool have_post_lut = false;
  bool post_lut_blending = false;
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
  float rotor_heat = 0.0f;  // thermal brake rotor glow [0..1] (heats under braking at speed, cools smoothly)
  HubCamera hub_cam;
  // media/UICarTargetCameraPresets.xml, read on first use (MENU-03).
  std::vector<carview::CarCameraPreset> hub_cam_presets;
  bool hub_cam_presets_read = false;
  const carview::CarCameraPreset* hub_preset(std::string_view name);
  // media/camera.zip's animated sequences by member (HomeSpaceCams.xml,
  // WorkshopCams.xml), read on first use (MENU-03); null when missing.
  std::map<std::string, std::unique_ptr<cutscene::Library>> hub_cam_sequences;
  const cutscene::Cutscene* hub_sequence(std::string_view member, std::string_view name);
  HubRoom last_hub_room = HubRoom::kNone;
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
  int live_hud_speedo_mode = 0;  // 0=Analog, 1=Digital, 2=Off
  int live_minimap_mode = 0;     // 0=Rotating, 1=North-Up, 2=Off
  HudToggles live_hud;           // the original's HUD options

  WheelPose poses[4]{};
  CameraPull camera_pull;
  drive::DriveCamera rig;
  bool have_rig = false;
  drive::GamepadRumble gamepad_rumble;
  drive::EngineSputter engine_sputter;
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
  bool garage_barn_finds = false;  // the garage shows its BARN FINDS list
  // The screen the garage was opened from, which B returns to (the
  // workshop's list stays open underneath on its own).
  enum class GarageFrom { kNone, kOutpostHub, kPause } garage_from = GarageFrom::kNone;
  PauseMenu::Screen garage_pause_screen = PauseMenu::Screen::kList;  // kPause: the rows
  int garage_pause_selected = 0;                                       // and the row
  size_t garage_pick = 0;
  bool grid_menu_open = false;
  double pre_race_blend_t = -1.0;
  int grid_menu_selected = 0;
  // The original's screens around a race (race_flow.h): the marker's event
  // summary and car select (entry_screens), the grid and the rewards
  // (race_screens).
  race_flow::Flow entry_screens;
  race_flow::Flow race_screens;
  bool race_quitting = false;  // QUIT RACE: the black-out runs, then the load
  // The pre-race rival callout (prerace_nemesis): the field's first star
  // driver (AIPlayers.Nemesis) and its BEAT THE STAR reward.
  std::string race_rival_name;
  int64_t race_rival_reward = 0;
  // The race's cutscenes (the flows' CPrepareCutscene: prerace_intro under
  // the title card, prerace_looping_01 under the grid menu, nemesis_show,
  // prerace_countdown, then postrace_finishline and the transitions under
  // the rewards): their own runtime, the track's set (Tracks.DevName).
  CutsceneRuntime race_cuts;
  std::string race_cut_name;  // the cutscene the flow asked for last
  // A free-roam head-to-head's screens (race_encounters.xml): the intro as
  // it starts (H_RACE_ENCOUNTER), the banner and the cash on a win.
  race_flow::Flow h2h_screens;
  std::string h2h_name, h2h_make, h2h_car, h2h_class;
  int h2h_pi = 0;
  int64_t h2h_reward = 0;
  bool h2h_was_active = false;
  double h2h_finish_seen = -1e9;
  const race::CareerEvent* summary_event = nullptr;
  bool summary_restrictions = false;
  EventSummaryHud event_summary_hud;
  double summary_opened_at = 0;
  // Car select (108_C_BUY_CARSELECT): the garage's cars that qualify.
  struct EventCar {
    std::string name, media;
    int64_t car_id = 0;
    double raw_pi = -1;
  };
  std::vector<EventCar> event_cars;
  int event_car_selected = 0;
  bool event_car_specs = true;  // Y TOGGLE: the selected car's panel
  std::string event_car_media;  // the car chosen for the event
  bool stay_in_car_pending = false;
  std::string stay_in_car_name, stay_in_car_media;  double title_orbit_time = 0.0;
  double title_idle_s = 0.0;
  bool title_attract_playing = false;
  LoadingScreen loading_screen;
  double loading_min_until = -1.0;
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
  bool map_open = false, map_dirty = false, map_dragging = false, map_drag_moved = false;
  float map_drag_start_x = 0.0f, map_drag_start_y = 0.0f;
  double map_zoom = 2, map_cx = 0, map_cz = 0;
  MapFilterSet map_filter;
  // The FILTER popup (LB on the map) and its cursor row.
  bool map_filter_open = false;
  int map_filter_row = 0, map_filter_first = 0;
  // The scene-driven pause map (drive_map_screen.cpp): the free cursor, the
  // world point under it, the opening zoom and the fast travel dialog.
  MapScreenHud map_screen_hud;
  MapCursor map_cursor;
  double map_opened_at = 0.0, map_base_zoom = 2.0;
  // The icon under the cursor (update_map_selection), and the pause map's
  // icon animation clock (MapScene's animation fields, map_anim_fill).
  bool map_hover_on = false;
  double map_hover_x = 0.0, map_hover_z = 0.0;
  bool map_anim_hover = false, map_anim_prev = false;
  double map_anim_hx = 0.0, map_anim_hz = 0.0, map_anim_px = 0.0, map_anim_pz = 0.0;
  double map_anim_hover_since = 0.0, map_anim_ghost_since = -1.0;
  double map_anim_drawn = -1.0;  // the map time last drawn
  // A barn rumour's reveal (barnfinds.xml barnfindspawn_root): the map opens
  // paused with Barn Finds forced on, Dak's VO plays and the camera runs the
  // barn's map animation from the player to the rumour; the cursor is the
  // player's again at its set_cursor trigger.
  const mapanim::Animation* map_reveal_anim = nullptr;
  double map_reveal_t = 0.0;
  bool map_reveal_locked = false;
  double map_reveal_cx = 0.0, map_reveal_cz = 0.0, map_reveal_zoom = 1.0;  // the opening camera
  bool map_reveal_restore_filter = false;
  MapFilterSet map_reveal_saved_filter;
  double map_px = 0.0, map_pz = 0.0;
  MapDialog map_dialog = MapDialog::kNone;
  int map_dialog_marker = -1;
  int64_t map_dialog_cost = 0;
  double map_dialog_px = 0.0, map_dialog_pz = 0.0;  // the trainer's bare-cursor destination
  // X DETAILS on an event: its ticket (H_EVENT_SUMMARY) over the map, with
  // A SET ROUTE, B BACK and Y RESTRICTIONS (map_popup_event_details_orig.png).
  bool map_details = false;
  bool map_details_restrictions = false;
  EventSummaryHud map_details_hud;
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
  ClubMenuData build_club_menu_data();
  ClubHud build_club_hud();
  // The pause row: the menus (the intro video on the first visit).
  void club_open_menu();
  // A button on the open menus.
  void club_menu_press(ClubMenu::Button b);
  // BEGIN CHALLENGES IN THIS CAR: the record, the save, and the car swap
  // when it is not the driven one.
  void club_begin_in(int64_t car_id);

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
  // The Sponsor Challenges rows (sponsor data + the profile's progress).
  std::vector<SponsorChallengeRow> sponsor_rows();
  // The pause menu's keys and buttons (EXTRAS, the screens it opens); true
  // when the event was the menu's.
  bool pause_back();
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
  // The garage (CHANGE CAR); `barn_finds`: its BARN FINDS list.
  void open_garage_browser(bool barn_finds);
  // B on the garage: back one screen, to where it was opened from.
  void close_garage_browser();
  void sync_livery_preview(bool recomposite_vinyls);
  void open_paint();
  void open_shop_or_autoshow(bool is_autoshow);
  // The original's H_GENERIC option menus (hub_menus.h): MY PROFILE,
  // OPTIONS, the paint booth, the workshop, the photo mode select.
  // The original's car list (108_C_BUY_CARSELECT) for the Autoshow's car
  // select and the garage; true while the HUD draws it (its help bar's hit
  // order differs from the engine's pages).
  bool car_list_drawn = false;
  std::map<int, std::string> country_flags;  // List_Country.CountryID -> flag texture key
  bool country_flags_built = false;
  const std::string& country_flag(int country_id);
  // Data_Car's rows as the 108 panel shows them: ModelShort ("XKR-S"), the
  // year, the make's name, the country, the engine's place, the drive type
  // and the ratings.
  struct CarListInfo {
    std::string model, make;
    int year = 0, country = 0, placement = 1, drive_type = 0;
    double raw_pi = 0;
    std::array<double, 5> stats{};
  };
  std::map<int64_t, CarListInfo> car_list_infos;
  bool car_list_infos_built = false;
  const CarListInfo* car_list_info(int64_t car_id);
  // The panel for a car; `raw_pi` < 0: the car's stock index.
  carview::CarListDetails car_list_details(int64_t car_id, double raw_pi = -1);
  void fill_autoshow_list(carview::CarListHud& out);
  // List_CarMake's logo (IconPath) and profile (Profile) by make id.
  struct MakeInfo {
    std::string logo, profile;
  };
  std::map<int64_t, MakeInfo> make_infos;
  bool make_infos_built = false;
  const MakeInfo* make_info(int64_t make_id);
  // The Autoshow's manufacturer select on 115_C_BUY_MFRSELECT.
  void fill_make_list(carview::MakeListHud& out);
  bool make_list_drawn = false;
  bool make_none_affordable = false;  // Y with nothing affordable: NONE AFFORDABLE
  // MY GARAGE and BARN FINDS on 108 (garage_barn_finds picks the list).
  void fill_garage_list(carview::CarListHud& out);
  bool garage_show_specs = true;  // Y TOGGLE
  size_t barn_pick = 0;           // the Barn Finds list's cursor (Collectibles::barns index)
  // The open popup over the garage's lists.
  enum class GaragePopup : uint8_t { kNone, kCollect, kNotReady, kNotDiscovered, kCantSell };
  GaragePopup garage_popup = GaragePopup::kNone;
  // Whether a Data_Car id is a barn find's car (it cannot be sold).
  bool is_barn_car(int64_t car_id) const;
  // Barn Finds: A on the selected barn, the popup's answers.
  void barn_select();
  void garage_popup_accept();
  void garage_popup_view_map();
  carview::HubMenuStack hub_menu;
  bool hub_menu_from_pause = false;  // B on its last list returns to the pause menu
  void open_hub_menu(carview::HubMenu m);
  // A on the selected row (or X: the menu's engine extra).
  void hub_menu_choose(bool extra);
  // The settings page, on `tab` (SettingsPage::Tab; -1: the last one).
  void open_settings(int tab);
  // OPTIONS's DIFFICULTY and HUD on the original's page
  // (300_I_OPT_ASSISTS_DIFFICULTY): the settings page's rows by name, with
  // A SAVE, B CANCEL (the rows as they were) and Y RESET TO DEFAULT.
  carview::OptionsPage options_page = carview::OptionsPage::kNone;
  int options_row = 0;
  std::vector<std::string> options_snapshot;
  void open_options_page(carview::OptionsPage p);
  void fill_options(carview::OptionsHud& out);
  // MY PROFILE > STATS on H_STATISTICS, over the career page (its tab
  // kStats): the tab shown, -1 when the page is the engine's.
  int stats_tab = -1;
  carview::StatsData stats_data;
  void open_stats();
  void enter_photo_mode();
  bool collect_next_wristband();
  void open_events_browser(int hub_filter);
  bool try_enter_event(const race::CareerEvent& ev, const std::string& chosen_car_media = {});
  // The marker's H_EVENT_SUMMARY (festival_event_entry_flow): A enters (car
  // select first when the car is not eligible), B backs out, Y restrictions.
  void open_event_summary(const race::CareerEvent& ev);
  // The event's ticket (H_EVENT_SUMMARY's values) for the current car; with
  // `cars`, the garage's cars that can enter it when the current one cannot.
  EventSummaryHud event_ticket(const race::CareerEvent& ev,
                               std::vector<EventCar>* cars = nullptr);
  void close_event_summary();
  void event_summary_accept();
  void event_summary_back();
  void event_summary_move(int by);
  void event_summary_restrictions();
  bool event_summary_open() const { return entry_screens.active(); }
  // A message popup over the game (STAY IN CAR, the pause menu's
  // confirmations): the frame behind it is blurred.
  bool popup_backdrop() const;
  // Advances the race screens (timed steps, the grid's start, the finish).
  void update_race_screens(double dt);
  // Plays the race cutscene the flow's step asks for (update_race_screens).
  void sync_race_cutscene();
  // The head-to-head's screens, from the festival racers' state.
  void sync_h2h_screens(double dt);
  // The race cutscene's camera this frame (false: the game's own camera).
  bool race_cutscene_camera(drive::CameraView& out) const;
  void grid_menu_accept();
  void race_back();
  void finish_race_screens();
  void begin_race_quit();
  void update_driving_line_overlay();
  void start_new_career();
  bool map_marker_visible(const MapMarker& m);
  void update_map_selection();
  void open_map();
  // First-time career glue (drive_first_time_career.cpp).
  void feed_first_time_career();
  void apply_first_time_career();
  double ftc_throttle = 0.0;
  bool ftc_handbrake = false;
  // While the career drives (controls off, AI on): the engine's AI driver on
  // the sat-nav route, one car. Rebuilt when the route changes.
  struct FtcAutopilot;
  std::shared_ptr<FtcAutopilot> ftc_autopilot;
  drive::Input ftc_autopilot_input();
  double ftc_last_impact_seen = -10.0;
  void close_map();
  // The scene-driven pause map (drive_map_screen.cpp).
  void map_screen_open();
  void map_screen_step(double dt, float sx, float sy, float zoom_axis);
  void map_screen_fill();
  // The pause map's icon animations: fills `ms`'s animation fields and plays
  // the player's radar sound; due when the next 30 Hz animation frame is.
  void map_anim_fill(MapScene& ms);
  bool map_anim_due() const;
  // The rumour reveal: opens the map on barn `index`'s animation; steps its
  // camera (true while the player's input is held).
  void map_reveal_start(size_t index);
  bool map_reveal_step(double dt);
  bool map_screen_event(const SDL_Event& e);
  void map_cursor_world();
  bool map_travel_quote(const MapMarker& m, int64_t& cost);
  bool map_marker_is_here(const MapMarker& m) const;  // the festival site the player is at
  int64_t outpost_travel_fee(const std::string& outpost_id);  // the original's fee (freeroam/fast_travel.h)
  void map_fast_travel_request();
  void map_fast_travel_confirm();
  bool map_open_details();
  void map_route_to_selected();
  void map_travel_to(double x, double z, double heading);
  void sync_pause_labels();
  void cycle_difficulty(int by);
  void choose();
  // A on the pause menu's confirmation popup: runs its action (QUIT, QUIT RACE).
  void confirm_pause_popup();
  void run_pause_action(PauseMenu::Action act);
  void race_continue();
  void race_restart();
  // Commits the finished race's held award (career, garage, campaign tally,
  // save); false when none is held.
  bool commit_race_award();
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
  void poll_async_guest_loads();
  void request_guest_load_async(const std::string& key, const std::string& media, const ShowroomOptions& opt,
                                size_t max_cache = 36);
  void apply_story_action(const StoryAction& sa);
  std::string find_baked_car(const std::string& media, int64_t car_id);
  void apply_campaign_action(const race::CampaignAction& ca);
  bool render_window_frame(uint64_t now, double frame_dt, bool menu_paused, bool trainer_freecam_active);
  void run_window_loop();
};

}  // namespace carview
