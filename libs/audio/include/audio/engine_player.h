// Plays a car's engine loops: per audio update the loops of each group are
// chosen, faded and pitched for the engine rpm (engine_sound.h), and the
// voices are mixed to stereo at the output rate, the loops' baked PCM
// resampled by linear interpolation.
// Spec: SPEC-ENGINE-SOUND "Engine loops", "Mix basics", "Rates and timing".
// Ours: gain and pitch move to their targets over about 5 ms; a voice
// starts at its sample's beginning; the low-pass is one pole.
#pragma once

#include <vector>

#include "audio/engine_sound.h"

namespace audio {

struct PcmSample {
  std::vector<float> data;  // mono, -1..1; loops over its whole length
  int rate = 0;
};

class EnginePlayer {
 public:
  struct Update {
    double rpm = 0;         // physics rpm
    double throttle = 0;
    double pos_torque = 0;
    double neg_torque = 0;
    View view = View::kFollow;
  };

  // `pcm[g][i]`: group g's loop i (null: silent). `idle` and `redline`:
  // the physics rpm range of the rpm map.
  EnginePlayer(const EngineSound& sound, std::vector<std::vector<const PcmSample*>> pcm, int out_rate, double idle,
               double redline);

  void set(const Update& u);
  // Mixes `frames` stereo frames (interleaved L, R) into `out` (overwrites).
  void render(float* out, int frames);

  bool neg_raises = false;  // the open NegTorque sign (spec: a switch)
  double master = 1;        // our overall level, set by ear

  double audio_rpm() const { return audio_rpm_; }
  int voices() const;

 private:
  struct Voice {
    bool active = false;
    double phase = 0, gain = 0, step = 0;
    double target_gain = 0, target_step = 0;
  };
  struct GroupState {
    std::vector<Voice> voices;
    double gain = 0, target_gain = 0;
    double left = 1, right = 1;
    double cutoff = 0, lp = 0;  // Hz (0: no filter), the filter's state
  };
  const EngineSound& sound_;
  std::vector<std::vector<const PcmSample*>> pcm_;
  std::vector<GroupState> state_;
  int rate_;
  double idle_, redline_;
  double audio_rpm_ = 0;
  double smooth_, fade_;
};

}  // namespace audio
