#include "story_intro.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "hud_scene.h"
#include "ui4_chrome.h"

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
         "I'm Dak — your 1995 VW Corrado VR6 is parked in your garage with a 15,000 Cr bonus. Bring it by anytime for upgrades or barn finds!",
         -1.0, 5.5, 5.5},
        {"ALICE HART", "AliceOpeningInterview1", "",
         "Step into the Horizon Autoshow to check out curated affordable starter rides, or keep your Corrado VR6 and chase your Green Wristband!",
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
      objective_detail_ = "1995 VOLKSWAGEN CORRADO VR6 — HORIZON QUALIFICATION";
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
      objective_title_ = "FIRST FESTIVAL CAR";
      objective_detail_ = "WELCOME TO THE AUTOSHOW — KEEP YOUR CORRADO VR6 OR PICK A CURATED STARTER RIDE";
      act.swap_car = true;
      act.car_media = kCorradoMedia;
      act.car_id = kCorradoCarId;
      act.grant_starter = true;
      act.open_autoshow = true;
      act.intro_finished = true;
      act.onboarding_banner =
          "WELCOME TO HORIZON AUTOSHOW — 1995 VW CORRADO VR6 IN GARAGE + 15,000 CR STARTER BONUS";
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

  // 947_HUD/HUDInstruction: slim sharp FH1 tilted tape + HUDPILL01 strips (ZERO 4px rounded boxes!).
  std::string sub = objective_detail_;
  if (stage_ == StoryStage::kViperDrive) {
    char dbuf[64];
    if (dist_to_festival_ >= 1000.0)
      std::snprintf(dbuf, sizeof dbuf, "HORIZON FESTIVAL  •  %.1f KM REMAINING", dist_to_festival_ / 1000.0);
    else
      std::snprintf(dbuf, sizeof dbuf, "HORIZON FESTIVAL  •  %d M REMAINING", int(std::lround(dist_to_festival_)));
    sub = dbuf;
  }
  ui::TextStyle title_ts = ui4::style(ui::Font::kE, 19.0f * s, ui4::kWhite);
  ui::TextStyle sub_ts = ui4::style(ui::Font::kB, 13.5f * s, ui4::kWhite);

  const float x0 = 64.0f * s, y0 = 38.0f * s;
  const float tw = std::max(280.0f * s, ui::List::measure(objective_title_, title_ts) + 38.0f * s);
  const float sw = std::max(260.0f * s, ui::List::measure(sub, sub_ts) + 34.0f * s);
  {
    ui4::TiltScope tilt(l, x0 + 0.5f * tw, y0 + 26.0f * s, ui4::kTiltRad * 0.5f);
    ui4::brush(l, x0 - 6.0f * s, y0, x0 + tw, y0 + 32.0f * s, ui4::kPink, false);
    l.text(x0 + 14.0f * s, y0 + 7.0f * s, objective_title_, title_ts);
    if (!sub.empty()) {
      ui4::pill(l, x0, y0 + 34.0f * s, x0 + sw, y0 + 60.0f * s, ui4::kPillGrey, ui4::PillKind::kHud01);
      l.text(x0 + 14.0f * s, y0 + 40.0f * s, sub, sub_ts);
    }
  }

  // Top-right skip hint on slim sharp HUDPILL01 (zero rounded corners).
  const float sx1 = screen_w - 64.0f * s;
  ui4::pill(l, sx1 - 210.0f * s, y0, sx1, y0 + 28.0f * s, ui4::kPillGrey, ui4::PillKind::kHud01);
  l.text(sx1 - 12.0f * s, y0 + 8.0f * s, "[TAB] SKIP STORY STAGE",
         ui4::style(ui::Font::kB, 12.0f * s, ui4::kDim, ui::Align::kRight));

  // Bottom character VO caption on sharp HUDPILL01 + pink speaker brush (zero 4px rounded boxes!).
  if (!active_caption_.empty()) {
    const float cx0 = 64.0f * s;
    const float cw = std::min(screen_w - 340.0f * s, 820.0f * s);
    ui::TextStyle cap_ts = ui4::style(ui::Font::kA, 14.5f * s, ui4::kWhite, ui::Align::kLeft, true);

    const float max_line_w = std::max(180.0f * s, cw - 32.0f * s);
    std::string wrapped;
    std::string cur;
    size_t pos = 0;
    while (pos < active_caption_.size()) {
      while (pos < active_caption_.size() && active_caption_[pos] == ' ') ++pos;
      size_t end = active_caption_.find(' ', pos);
      if (end == std::string::npos) end = active_caption_.size();
      const std::string word = active_caption_.substr(pos, end - pos);
      pos = end;
      if (word.empty()) continue;
      const std::string cand = cur.empty() ? word : (cur + " " + word);
      if (!cur.empty() && ui::List::measure(cand, cap_ts) > max_line_w) {
        if (!wrapped.empty()) wrapped.push_back('\n');
        wrapped += cur;
        cur = word;
      } else {
        cur = cand;
      }
    }
    if (!cur.empty()) {
      if (!wrapped.empty()) wrapped.push_back('\n');
      wrapped += cur;
    }

    const float cap_h = ui::List::measure_height(wrapped, cap_ts);
    const float box_h = std::max(44.0f * s, cap_h + 20.0f * s);
    const float cy0 = screen_h - 46.0f * s - box_h;
    if (!active_speaker_.empty()) {
      ui::TextStyle sp_ts = ui4::style(ui::Font::kE, 13.0f * s, ui4::kWhite);
      const float spw = ui::List::measure(active_speaker_, sp_ts) + 28.0f * s;
      ui4::brush(l, cx0 - 4.0f * s, cy0 - 24.0f * s, cx0 + spw, cy0 + 2.0f * s, ui4::kPink, false);
      l.text(cx0 + 10.0f * s, cy0 - 18.0f * s, active_speaker_, sp_ts);
    }
    ui4::pill(l, cx0, cy0, cx0 + cw, cy0 + box_h, ui4::kCharcoalDark, ui4::PillKind::kHud01);
    l.rect(cx0, cy0, cx0 + 4.0f * s, cy0 + box_h, ui4::kPink);
    l.text(cx0 + 16.0f * s, cy0 + 10.0f * s, wrapped, cap_ts);
  }
}

}  // namespace carview
