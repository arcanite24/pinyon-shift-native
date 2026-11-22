#include "story_intro.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace carview {

const char* StoryIntro::stage_name(StoryStage s) {
  switch (s) {
    case StoryStage::kInactive: return "Inactive";
    case StoryStage::kBootSplash: return "Boot Splash";
    case StoryStage::kOpeningFmv: return "Opening FMV";
    case StoryStage::kViperDrive: return "Viper Drive to Horizon";
    case StoryStage::kCorradoQualify: return "Corrado Qualification Race";
    case StoryStage::kFestivalIntroBeats: return "Festival Welcome & Wristband";
    case StoryStage::kAutoshowHandoff: return "Autoshow Handoff";
    case StoryStage::kCompleted: return "Completed";
  }
  return "Unknown";
}

StoryAction StoryIntro::begin_boot(const std::filesystem::path& game_root, bool skip_intro_movies,
                                   bool start_in_single_player, bool needs_new_career_intro) {
  game_root_ = game_root;
  skip_movies_ = skip_intro_movies;
  start_sp_ = start_in_single_player;
  pending_career_intro_ = needs_new_career_intro;
  splash_queue_.clear();
  splash_idx_ = 0;

  if (!skip_movies_) {
    // Queue available boot splash movies from the disc.
    for (FmvMovieId id : {FmvMovieId::kT10MsCombined, FmvMovieId::kDolbyCorona, FmvMovieId::kForzaIntro}) {
      if (!FmvPlayer::resolve_movie(game_root_, id).empty()) splash_queue_.push_back(id);
    }
  }
  if (!splash_queue_.empty()) {
    return enter_stage(StoryStage::kBootSplash);
  }
  if (start_sp_) {
    if (pending_career_intro_) return begin_new_career(game_root_, skip_movies_, festival_gate_);
    stage_ = StoryStage::kInactive;
    return {};
  }
  stage_ = StoryStage::kInactive;
  StoryAction act;
  act.enter_title_screen = true;
  return act;
}

StoryAction StoryIntro::begin_new_career(const std::filesystem::path& game_root, bool skip_intro_movies,
                                         std::array<double, 3> festival_gate) {
  game_root_ = game_root;
  skip_movies_ = skip_intro_movies;
  festival_gate_ = festival_gate;
  qualify_launched_ = false;
  ceremony_fmv_triggered_ = false;
  if (!skip_movies_ && !FmvPlayer::resolve_movie(game_root_, FmvMovieId::kFmv01Opening).empty()) {
    return enter_stage(StoryStage::kOpeningFmv);
  }
  return enter_stage(StoryStage::kViperDrive);
}

void StoryIntro::populate_beats_for(StoryStage s) {
  beats_.clear();
  beat_idx_ = 0;
  active_speaker_.clear();
  active_caption_.clear();
  caption_timer_ = 0.0;

  if (s == StoryStage::kViperDrive) {
    beats_ = {
        {"SCOTT TYLER", "AliceOpeningInterview1", "AllComersEventActivated",
         "Welcome to Horizon Bass Arena! Alice Hart says only 250 festival passes remain — get to the Horizon Festival!",
         -1.0, 0.5, 7.0},
        {"ALICE HART", "FestivalTicketsDepleting9", "",
         "Festival wristbands are going fast — keep the SRT Viper moving down the highway toward the main gate!",
         2200.0, 14.0, 6.5},
        {"SCOTT TYLER", "AliceOpeningInterview3", "",
         "You can hear the main stage from here! Pull into the Horizon Festival entrance to claim your qualifying spot.",
         900.0, 28.0, 6.5},
    };
  } else if (s == StoryStage::kCorradoQualify) {
    beats_ = {
        {"ALICE HART", "AliceOpeningInterview2A", "",
         "Take the VW Corrado VR6 to the grid and finish the qualification race to earn your Yellow Wristband!",
         -1.0, 0.5, 7.0},
    };
  } else if (s == StoryStage::kFestivalIntroBeats) {
    beats_ = {
        {"ALICE HART", "", "230PopularityReached",
         "You made it! Here is your official Yellow Wristband — welcome to the Horizon Festival!",
         -1.0, 0.2, 5.0},
        {"DAK", "", "1040BarnFindAtWorkshop",
         "I'm Dak — bring your rides to the Horizon Garage anytime for upgrades, tuning, or barn find restorations.",
         -1.0, 5.5, 5.5},
        {"SCOTT TYLER", "AliceOpeningInterview1", "",
         "Head over to the Autoshow to pick up your festival ride and hit the Colorado roads!",
         -1.0, 11.2, 5.0},
    };
  }
}

StoryAction StoryIntro::enter_stage(StoryStage next) {
  stage_ = next;
  stage_time_ = 0.0;
  populate_beats_for(next);
  StoryAction act;

  switch (next) {
    case StoryStage::kBootSplash:
      objective_title_ = "BOOT SPLASH";
      objective_detail_.clear();
      if (splash_idx_ < int(splash_queue_.size())) {
        act.play_fmv = true;
        act.fmv_id = splash_queue_[size_t(splash_idx_)];
      }
      break;

    case StoryStage::kOpeningFmv:
      objective_title_ = "HORIZON FESTIVAL";
      objective_detail_ = "OPENING MONTAGE";
      act.play_fmv = true;
      act.fmv_id = FmvMovieId::kFmv01Opening;
      break;

    case StoryStage::kViperDrive:
      objective_title_ = "DRIVE TO THE HORIZON FESTIVAL";
      objective_detail_ = "2013 SRT VIPER GTS — CLAIM YOUR FESTIVAL WRISTBAND";
      act.swap_car = true;
      act.car_media = kViperMedia;
      act.car_id = kViperCarId;
      act.teleport = true;
      act.pos = viper_spawn_;
      act.heading = viper_heading_;
      act.set_gps_target = true;
      act.gps_xz = {festival_gate_[0], festival_gate_[2]};
      dist_to_festival_ = std::hypot(festival_gate_[0] - viper_spawn_[0], festival_gate_[2] - viper_spawn_[2]);
      break;

    case StoryStage::kCorradoQualify:
      objective_title_ = "QUALIFICATION RACE";
      objective_detail_ = "1995 VOLKSWAGEN CORRADO VR6 — FESTIVAL FIRST";
      act.swap_car = true;
      act.car_media = kCorradoMedia;
      act.car_id = kCorradoCarId;
      act.launch_qualify_race = true;
      act.qualify_route_id = kQualifyRouteId;
      qualify_launched_ = true;
      break;

    case StoryStage::kFestivalIntroBeats:
      objective_title_ = "WELCOME TO HORIZON";
      objective_detail_ = "YELLOW WRISTBAND AWARDED";
      if (!skip_movies_ && !ceremony_fmv_triggered_ &&
          !FmvPlayer::resolve_movie(game_root_, FmvMovieId::kWristbandGet).empty()) {
        ceremony_fmv_triggered_ = true;
        act.play_fmv = true;
        act.fmv_id = FmvMovieId::kWristbandGet;
      }
      break;

    case StoryStage::kAutoshowHandoff:
      objective_title_ = "HORIZON AUTOSHOW";
      objective_detail_ = "CHOOSE YOUR FESTIVAL RIDE";
      act.swap_car = true;
      act.car_media = kCorradoMedia;
      act.car_id = kCorradoCarId;
      act.grant_starter = true;
      act.open_autoshow = true;
      act.intro_finished = true;
      stage_ = StoryStage::kCompleted;
      break;

    case StoryStage::kInactive:
    case StoryStage::kCompleted:
      objective_title_.clear();
      objective_detail_.clear();
      break;
  }
  return act;
}

StoryAction StoryIntro::update(double dt, std::array<double, 2> player_xz, bool fmv_active, bool race_finished) {
  if (!active()) return {};
  dt = std::max(0.0, dt);
  stage_time_ += dt;
  if (caption_timer_ > 0.0) {
    caption_timer_ = std::max(0.0, caption_timer_ - dt);
    if (caption_timer_ <= 0.0) {
      active_speaker_.clear();
      active_caption_.clear();
    }
  }

  StoryAction act;
  switch (stage_) {
    case StoryStage::kBootSplash:
      if (!fmv_active) {
        ++splash_idx_;
        if (splash_idx_ < int(splash_queue_.size())) {
          act.play_fmv = true;
          act.fmv_id = splash_queue_[size_t(splash_idx_)];
          return act;
        }
        if (start_sp_) {
          if (pending_career_intro_) return begin_new_career(game_root_, skip_movies_, festival_gate_);
          stage_ = StoryStage::kInactive;
          return {};
        }
        stage_ = StoryStage::kInactive;
        act.enter_title_screen = true;
        return act;
      }
      return {};

    case StoryStage::kOpeningFmv:
      if (!fmv_active) return enter_stage(StoryStage::kViperDrive);
      return {};

    case StoryStage::kViperDrive: {
      dist_to_festival_ = std::hypot(festival_gate_[0] - player_xz[0], festival_gate_[2] - player_xz[1]);
      if (beat_idx_ < int(beats_.size())) {
        const StoryBeat& b = beats_[size_t(beat_idx_)];
        const bool by_dist = b.trigger_dist_m >= 0.0 && dist_to_festival_ <= b.trigger_dist_m;
        const bool by_time = stage_time_ >= b.trigger_time_s;
        if (by_dist || by_time) {
          active_speaker_ = b.speaker;
          active_caption_ = b.caption;
          caption_timer_ = b.duration_s;
          act.radio_immediate_event = b.radio_event;
          act.vo_trigger = b.vo_trigger;
          ++beat_idx_;
        }
      }
      if (dist_to_festival_ <= kFestivalArrivalRadiusM) {
        return enter_stage(StoryStage::kCorradoQualify);
      }
      return act;
    }

    case StoryStage::kCorradoQualify:
      if (beat_idx_ < int(beats_.size()) && stage_time_ >= beats_[size_t(beat_idx_)].trigger_time_s) {
        const StoryBeat& b = beats_[size_t(beat_idx_)];
        active_speaker_ = b.speaker;
        active_caption_ = b.caption;
        caption_timer_ = b.duration_s;
        act.radio_immediate_event = b.radio_event;
        act.vo_trigger = b.vo_trigger;
        ++beat_idx_;
      }
      if (race_finished && qualify_launched_) {
        return enter_stage(StoryStage::kFestivalIntroBeats);
      }
      return act;

    case StoryStage::kFestivalIntroBeats:
      if (fmv_active) return {};
      if (beat_idx_ < int(beats_.size()) && stage_time_ >= beats_[size_t(beat_idx_)].trigger_time_s) {
        const StoryBeat& b = beats_[size_t(beat_idx_)];
        active_speaker_ = b.speaker;
        active_caption_ = b.caption;
        caption_timer_ = b.duration_s;
        act.radio_immediate_event = b.radio_event;
        act.vo_trigger = b.vo_trigger;
        ++beat_idx_;
      }
      if (beat_idx_ >= int(beats_.size()) && caption_timer_ <= 0.0 && stage_time_ >= 16.5) {
        return enter_stage(StoryStage::kAutoshowHandoff);
      }
      return act;

    case StoryStage::kAutoshowHandoff:
    case StoryStage::kInactive:
    case StoryStage::kCompleted:
      break;
  }
  return {};
}

StoryAction StoryIntro::skip_stage() {
  switch (stage_) {
    case StoryStage::kBootSplash:
      splash_idx_ = int(splash_queue_.size());
      if (start_sp_) {
        if (pending_career_intro_) return begin_new_career(game_root_, skip_movies_, festival_gate_);
        stage_ = StoryStage::kInactive;
        return {};
      }
      stage_ = StoryStage::kInactive;
      {
        StoryAction a;
        a.enter_title_screen = true;
        return a;
      }
    case StoryStage::kOpeningFmv: return enter_stage(StoryStage::kViperDrive);
    case StoryStage::kViperDrive: return enter_stage(StoryStage::kCorradoQualify);
    case StoryStage::kCorradoQualify: return enter_stage(StoryStage::kFestivalIntroBeats);
    case StoryStage::kFestivalIntroBeats: return enter_stage(StoryStage::kAutoshowHandoff);
    case StoryStage::kAutoshowHandoff:
    case StoryStage::kInactive:
    case StoryStage::kCompleted:
      return {};
  }
  return {};
}

StoryAction StoryIntro::skip_all() {
  if (!active()) return {};
  if (stage_ == StoryStage::kBootSplash) return skip_stage();
  return enter_stage(StoryStage::kAutoshowHandoff);
}

StoryAction StoryIntro::advance_to(StoryStage stage) { return enter_stage(stage); }

void StoryIntro::draw_hud(ui::List& l, float screen_w, float screen_h) const {
  if (!active() || stage_ == StoryStage::kBootSplash || stage_ == StoryStage::kOpeningFmv) return;
  const float s = screen_h / 720.0f;
  constexpr uint32_t kPink = ui::rgba(0xF5, 0x04, 0x5C);
  constexpr uint32_t kPanel = ui::rgba(10, 12, 18, 205);
  constexpr uint32_t kWhite = ui::rgba(248, 249, 252);
  constexpr uint32_t kDim = ui::rgba(195, 200, 212);

  // Top-left story objective card (Horizon pink accent strip + dark panel).
  const float x0 = 64.0f * s, y0 = 38.0f * s, w = 440.0f * s, h = 66.0f * s;
  l.rect(x0, y0, x0 + w, y0 + h, kPanel, 4.0f * s);
  l.rect(x0, y0, x0 + 6.0f * s, y0 + h, kPink);

  ui::TextStyle title_ts;
  title_ts.font = ui::Font::kE;
  title_ts.size = 20.0f * s;
  title_ts.color = kWhite;
  l.text(x0 + 18.0f * s, y0 + 10.0f * s, objective_title_, title_ts);

  std::string sub = objective_detail_;
  if (stage_ == StoryStage::kViperDrive) {
    char dbuf[64];
    if (dist_to_festival_ >= 1000.0)
      std::snprintf(dbuf, sizeof dbuf, "HORIZON FESTIVAL  •  %.1f KM REMAINING", dist_to_festival_ / 1000.0);
    else
      std::snprintf(dbuf, sizeof dbuf, "HORIZON FESTIVAL  •  %d M REMAINING", int(std::lround(dist_to_festival_)));
    sub = dbuf;
  }
  ui::TextStyle sub_ts;
  sub_ts.font = ui::Font::kB;
  sub_ts.size = 13.0f * s;
  sub_ts.color = kDim;
  l.text(x0 + 18.0f * s, y0 + 38.0f * s, sub, sub_ts);

  // Top-right skip hint for testing / fast-forward.
  const float sx1 = screen_w - 64.0f * s;
  l.rect(sx1 - 210.0f * s, y0, sx1, y0 + 30.0f * s, ui::rgba(10, 12, 18, 180), 4.0f * s);
  ui::TextStyle skip_ts;
  skip_ts.font = ui::Font::kB;
  skip_ts.size = 12.0f * s;
  skip_ts.color = kDim;
  skip_ts.align = ui::Align::kRight;
  l.text(sx1 - 12.0f * s, y0 + 9.0f * s, "[TAB] SKIP STORY STAGE", skip_ts);

  // Bottom-centre character VO caption box (Alice / Dak / Scott).
  if (!active_caption_.empty()) {
    const float cy0 = screen_h - 118.0f * s;
    const float cw = std::min(screen_w - 160.0f * s, 820.0f * s);
    const float cx0 = 0.5f * (screen_w - cw);
    l.rect(cx0, cy0, cx0 + cw, cy0 + 58.0f * s, ui::rgba(8, 10, 15, 210), 4.0f * s);
    if (!active_speaker_.empty()) {
      ui::TextStyle sp_ts;
      sp_ts.font = ui::Font::kE;
      sp_ts.size = 13.0f * s;
      sp_ts.color = kPink;
      l.text(cx0 + 16.0f * s, cy0 + 8.0f * s, active_speaker_, sp_ts);
    }
    ui::TextStyle cap_ts;
    cap_ts.font = ui::Font::kA;
    cap_ts.size = 14.5f * s;
    cap_ts.color = kWhite;
    cap_ts.keep_case = true;
    l.text(cx0 + 16.0f * s, cy0 + 28.0f * s, active_caption_, cap_ts);
  }
}

}  // namespace carview
