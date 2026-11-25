// FH1's driving tutor: the input hints a new player gets once the opening
// hands the car over (CActivateDrivingTutor). Read from the original's tutor
// update (2026-11-25): a small state machine over the tutor messages
// (CTutorialMessages, table HelpButtons: "PRESS <button> TO ACCELERATE",
// "... TO BRAKE", "... TO REWIND", "... TO USE E-BRAKE", "... TO CHANGE THE
// CAMERA"). Once activated it keeps running after the first-time career ends
// (nothing in the original turns it off); the rewind and e-brake hints stop
// after ten showings each, or for good once the player uses that control.
//
// Interim (SPEC_QUESTIONS Q63): the e-brake hint's turn test reads the
// original's guidance fields as "a left or right turn, normal or sharp"; the
// hint's on-screen time is the tutor state's own duration.
#pragma once

#include <cstdint>
#include <string>

namespace race {

// The CTutorialMessages entries the tutor shows (index in the table).
enum class TutorMessage : uint8_t {
  kNone = 0xFF,
  kRewind = 1,
  kBrake = 2,
  kAccelerate = 3,
  kMap = 4,
  kEBrake = 5,
  kCamera = 6,
};

struct TutorHint {
  const char* button = "";   // the controller button the hint shows (A, B, X, Y, LB, RB, LT, RT, BACK...)
  const char* left_id = "";  // HelpButtons string drawn before the glyph ("PRESS")
  const char* right_id = ""; // HelpButtons string drawn after it ("TO ACCELERATE")
};
// The hint for a message (map: with a Back button; the alternative without
// one is IDS_InputHint_Map_NoBackBtn_*).
TutorHint tutor_hint(TutorMessage m);

struct TutorInput {
  double dt = 0.0;
  double throttle = 0.0;     // 0..1, the player's
  double speed_mps = 0.0;
  bool impact = false;       // the player's car hit something this frame (the rewind request)
  bool rewind_available = false;
  bool rewind_pressed = false;  // the player used rewind this frame
  bool ebrake_pressed = false;  // the player pulled the e-brake this frame
  // The sat-nav's next manoeuvre: a left / right turn (normal or sharp) this far ahead.
  bool turn_ahead = false;
  double turn_distance_m = 0.0;
};

class DrivingTutor {
 public:
  // GlobalRegistry FirstTimeCareer tutor keys (BrakeDelay is the compiled default).
  static constexpr double kBrakeDelay = 10.0;
  static constexpr double kBrakeMessageTimeout = 5.0;
  static constexpr double kRewindMessageTimeout = 5.0;
  static constexpr double kEBrakeTimeout = 6.5;
  static constexpr double kChangeCameraMessageTimeout = 5.0;
  static constexpr double kChangeCameraMessageMinimalTimeout = 2.0;
  static constexpr double kRewindCoolDown = 30.0;
  static constexpr double kEBrakeCoolDown = 30.0;
  static constexpr int kTimesToShowRewind = 10;
  static constexpr int kTimesToShowEBrake = 10;
  static constexpr double kSpeedThresholdForEBrakeMph = 50.0;
  static constexpr double kDistanceThresholdForEBrake = 110.0;
  static constexpr double kThrottleThreshold = 0.01;

  void activate();
  bool active() const { return active_; }
  void update(const TutorInput& in);
  // The message on screen (kNone when none).
  TutorMessage message() const { return shown_; }
  int state() const { return state_; }

 private:
  void show(TutorMessage m);
  void hide() { shown_ = TutorMessage::kNone; }
  bool ebrake_due(const TutorInput& in) const;

  bool active_ = false;
  int state_ = 0;
  double timer_ = 0.0;
  double rewind_cool_ = 0.0, ebrake_cool_ = 0.0;
  int rewinds_left_ = kTimesToShowRewind, ebrakes_left_ = kTimesToShowEBrake;
  bool camera_shown_ = false;
  TutorMessage shown_ = TutorMessage::kNone;
};

}  // namespace race
