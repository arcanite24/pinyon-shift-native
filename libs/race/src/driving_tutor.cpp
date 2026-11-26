#include "race/driving_tutor.h"

#include <algorithm>

namespace race {

TutorHint tutor_hint(TutorMessage m) {
  // The button is the default layout's control for the action (the hint
  // table's glyph ids: accelerate, brake, rewind, map, e-brake, camera).
  switch (m) {
    case TutorMessage::kRewind: return {"Y", "IDS_InputHint_Rewind_L", "IDS_InputHint_Rewind_R"};
    case TutorMessage::kBrake: return {"LT", "IDS_InputHint_Brake_L", "IDS_InputHint_Brake_R"};
    case TutorMessage::kAccelerate: return {"RT", "IDS_InputHint_Accelerate_L", "IDS_InputHint_Accelerate_R"};
    case TutorMessage::kMap: return {"BACK", "IDS_InputHint_Map_L", "IDS_InputHint_Map_R"};
    case TutorMessage::kEBrake: return {"A", "IDS_InputHint_EBrake_L", "IDS_InputHint_EBrake_R"};
    case TutorMessage::kCamera: return {"RB", "IDS_InputHint_Camera_L", "IDS_InputHint_Camera_R"};
    case TutorMessage::kNone: break;
  }
  return {};
}

void DrivingTutor::activate() {
  *this = DrivingTutor{};
  active_ = true;
  timer_ = 0.0;
}

void DrivingTutor::show(TutorMessage m) {
  shown_ = m;
  timer_ = 0.0;
}

bool DrivingTutor::ebrake_due(const TutorInput& in) const {
  if (ebrake_cool_ > 0.0) return false;
  if (in.speed_mps * 2.2369 < kSpeedThresholdForEBrakeMph) return false;
  return in.turn_ahead && in.turn_distance_m > 0.0 && in.turn_distance_m <= kDistanceThresholdForEBrake;
}

void DrivingTutor::update(const TutorInput& in) {
  if (!active_) return;
  const double dt = std::max(0.0, in.dt);
  timer_ += dt;
  rewind_cool_ = std::max(0.0, rewind_cool_ - dt);
  ebrake_cool_ = std::max(0.0, ebrake_cool_ - dt);
  // Using the control ends its hints for good.
  if (in.rewind_pressed) rewinds_left_ = 0;
  if (in.ebrake_pressed) ebrakes_left_ = 0;
  // A rewind request is only taken while idle or showing the camera hint.
  const bool rewind_request = in.impact && (state_ == 4 || state_ == 7);
  switch (state_) {
    case 0:
      show(TutorMessage::kAccelerate);
      state_ = 1;
      break;
    case 1:
      if (in.throttle >= kThrottleThreshold) {
        hide();
        state_ = 2;
        timer_ = 0.0;
      }
      break;
    case 2:
      if (timer_ >= kBrakeDelay) {
        show(TutorMessage::kBrake);
        state_ = 3;
      }
      break;
    case 3:
      if (timer_ >= kBrakeMessageTimeout) {
        hide();
        state_ = 4;
      }
      break;
    case 4:
      if (rewind_request && rewinds_left_ > 0 && in.rewind_available) {
        // Still cooling down: nothing this frame (and no e-brake check).
        if (rewind_cool_ <= 0.0) {
          --rewinds_left_;
          rewind_cool_ = kRewindCoolDown;
          show(TutorMessage::kRewind);
          state_ = 5;
        }
      } else if (ebrakes_left_ > 0 && ebrake_due(in)) {
        --ebrakes_left_;
        ebrake_cool_ = kEBrakeCoolDown;
        show(TutorMessage::kEBrake);
        state_ = 6;
      }
      break;
    case 5:
      if (timer_ >= kRewindMessageTimeout || !in.rewind_available || in.rewind_pressed) {
        hide();
        state_ = 4;
      }
      break;
    case 6:
      if (in.ebrake_pressed) {
        hide();
        state_ = 4;
      } else if (timer_ >= kEBrakeTimeout) {
        hide();
        state_ = 4;
        if (!camera_shown_) {
          camera_shown_ = true;
          show(TutorMessage::kCamera);
          state_ = 7;
        }
      }
      break;
    case 7:
      if (rewind_request && rewinds_left_ > 0 && in.rewind_available && rewind_cool_ <= 0.0 &&
          timer_ >= kChangeCameraMessageMinimalTimeout) {
        --rewinds_left_;
        rewind_cool_ = kRewindCoolDown;
        show(TutorMessage::kRewind);
        state_ = 5;
      } else if (timer_ >= kChangeCameraMessageTimeout) {
        hide();
        state_ = 4;
      }
      break;
    default: state_ = 4; break;
  }
}

}  // namespace race
