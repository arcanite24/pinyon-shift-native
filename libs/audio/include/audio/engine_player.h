// Plays a car's engine loops with full Slice 2 harmonic DSP: per audio
// update the loops of each group are chosen, faded and pitched for the
// engine rpm (engine_sound.h), then filtered through resonant 2nd-order
// Lowpass, PEQ (with audio_rpm_idle_gain_reset on ExhaustL), LoadPEQ
// (PosLoad/NegLoad interpolated by throttle), and Expander (per-tailpipe
// burst coupled to exhaust burbles/backfires), plus one-shot IntakeChuff.
// Spec: SPEC-ENGINE-SOUND "Engine loops", "Harmonic and engine-tuning DSP
// stages", "Mix basics", "Rates and timing".
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
  // the physics rpm range of the rpm map. `intake_chuff`: optional one-shot PCM.
  EnginePlayer(const EngineSound& sound, std::vector<std::vector<const PcmSample*>> pcm, int out_rate, double idle,
               double redline, const PcmSample* intake_chuff = nullptr);

  void set_intake_chuff(const PcmSample* chuff) { chuff_pcm_ = chuff; }
  void set(const Update& u);
  // Triggers the Expander envelope on group `group` (or on all Exhaust groups
  // when `group < 0`), called when an exhaust burble or backfire pops.
  void trigger_expander(int group = -1);
  // Advances Expander timers by `dt` seconds (also advanced automatically by
  // render()).
  void advance_dsp(double dt);

  // Mixes `frames` stereo frames (interleaved L, R) into `out` (overwrites).
  void render(float* out, int frames);

  bool neg_raises = true;  // confirmed SPEC-ENGINE-SOUND: braking raises x
  double master = 1;       // overall level

  double audio_rpm() const { return audio_rpm_; }
  int voices() const;
  double expander_state(size_t group) const;
  double peq_gain(size_t group) const;
  double load_peq_gain(size_t group) const;
  bool intake_chuff_active() const { return chuff_active_; }

 private:
  struct Biquad {
    bool enabled = false;
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;
    void set_lowpass(double fc, double q, int rate);
    void set_peq(double fc, double bw_oct, double linear_gain, int rate);
    double tick(double x) {
      if (!enabled) return x;
      const double y = b0 * x + z1;
      z1 = b1 * x - a1 * y + z2;
      z2 = b2 * x - a2 * y;
      return y;
    }
  };
  struct Voice {
    bool active = false;
    double phase = 0, gain = 0, step = 0;
    double target_gain = 0, target_step = 0;
  };
  struct GroupState {
    std::vector<Voice> voices;
    double gain = 0, target_gain = 0;
    double base_target_gain = 0;
    double left = 1, right = 1;
    Biquad lp, peq, load_peq;
    double cur_peq_gain = 1.0, cur_load_peq_gain = 1.0;
    // Expander state (ms).
    bool exp_triggered = false;
    double exp_elapsed_ms = 0;
    double exp_max_gain = 0;
    double exp_state = 0;
  };
  void update_expander_state(size_t g, double dt_ms);

  const EngineSound& sound_;
  std::vector<std::vector<const PcmSample*>> pcm_;
  std::vector<GroupState> state_;
  int rate_;
  double idle_, redline_;
  double audio_rpm_ = 0;
  double smooth_, fade_;

  // IntakeChuff one-shot state.
  const PcmSample* chuff_pcm_ = nullptr;
  bool chuff_active_ = false;
  double chuff_phase_ = 0;
  double chuff_gain_ = 0;
  double chuff_left_ = 1, chuff_right_ = 1;
  bool have_prev_throttle_ = false;
  double prev_throttle_ = 0;
};

}  // namespace audio
