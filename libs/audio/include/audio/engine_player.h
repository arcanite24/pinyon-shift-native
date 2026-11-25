// Plays a car's engine loops with full Slice 2 harmonic DSP: per audio
// update the loops of each group are chosen, faded and pitched for the
// engine rpm (engine_sound.h), then filtered through resonant 2nd-order
// Lowpass, PEQ (with audio_rpm_idle_gain_reset on ExhaustL), LoadPEQ
// (PosLoad/NegLoad interpolated by throttle), and Expander (per-tailpipe
// burst coupled to exhaust burbles/backfires), plus one-shot IntakeChuff.
// Spec: SPEC-ENGINE-SOUND "Engine loops", "Harmonic and engine-tuning DSP
// stages", "Mix basics", "Rates and timing".
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
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

  // Optional lifetime pin (e.g. shared_ptr<const BakedSound>) kept alive until
  // this EnginePlayer is destroyed after audio-thread retirement.
  void attach_owner(std::shared_ptr<const void> owner) { owner_ = std::move(owner); }
  void set_intake_chuff(const PcmSample* chuff) { chuff_pcm_ = chuff; }
  void set(const Update& u);
  // Triggers the Expander envelope on group `group` (or on all Exhaust groups
  // when `group < 0`), called when an exhaust burble or backfire pops.
  void trigger_expander(int group = -1);
  // Triggers per-car ShiftVolumeScalar momentary volume boost on upshift/downshift.
  void trigger_shift_boost(bool upshift);
  // Advances Expander & shift-boost timers by `dt` seconds (also advanced
  // automatically by render()).
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
  double focus_peq_gain() const { return cur_focus_peq_gain_; }
  double distortion_level() const { return cur_dist_level_; }
  double shift_boost_gain() const { return std::clamp(1.0 + shift_boost_cur_, 0.2, 3.0); }
  bool intake_chuff_active() const { return chuff_active_; }

 private:
  struct Biquad {
    bool enabled = false;
    bool primed = false;
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double tb0 = 1, tb1 = 0, tb2 = 0, ta1 = 0, ta2 = 0;
    double z1 = 0, z2 = 0;
    void set_lowpass(double fc, double q, int rate);
    void set_peq(double fc, double bw_oct, double linear_gain, int rate);
    void set_bypass() {
      enabled = false;
      primed = false;
      b0 = tb0 = 1.0;
      b1 = b2 = a1 = a2 = 0.0;
      tb1 = tb2 = ta1 = ta2 = 0.0;
      z1 = z2 = 0.0;
    }
    double tick(double x, double smooth) {
      if (!std::isfinite(x)) x = 0.0;
      if (!enabled) {
        z1 = 0.0;
        z2 = 0.0;
        primed = false;
        return x;
      }
      if (!primed) {
        b0 = tb0;
        b1 = tb1;
        b2 = tb2;
        a1 = ta1;
        a2 = ta2;
        primed = true;
      } else {
        // Convex pole interpolation inside the Schur-Cohn stability triangle:
        // guaranteed stable and click-free across abrupt RPM/throttle jumps.
        b0 += (tb0 - b0) * smooth;
        b1 += (tb1 - b1) * smooth;
        b2 += (tb2 - b2) * smooth;
        a1 += (ta1 - a1) * smooth;
        a2 += (ta2 - a2) * smooth;
      }
      const double y = b0 * x + z1;
      z1 = b1 * x - a1 * y + z2;
      z2 = b2 * x - a2 * y;
      if (!std::isfinite(y) || !std::isfinite(z1) || !std::isfinite(z2) || std::fabs(y) > 1e4) {
        z1 = 0.0;
        z2 = 0.0;
        return 0.0;
      }
      if (std::fabs(z1) < 1e-18) z1 = 0.0;
      if (std::fabs(z2) < 1e-18) z2 = 0.0;
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

  // IntakeChuff one-shot state (33 ms sliding throttle history window).
  const PcmSample* chuff_pcm_ = nullptr;
  bool chuff_active_ = false;
  bool chuff_armed_ = true;
  double chuff_phase_ = 0;
  double chuff_gain_ = 0;
  double chuff_left_ = 1, chuff_right_ = 1;
  struct ThrottleHistEntry {
    double time_s = 0.0;
    double throttle = 0.0;
  };
  static constexpr int kThrottleHistSize = 16;
  std::array<ThrottleHistEntry, kThrottleHistSize> thr_hist_{};
  int thr_hist_head_ = 0;
  int thr_hist_count_ = 0;
  double dsp_time_s_ = 0.0;

  // Per-car ShiftVolumeScalar boost envelope state.
  double shift_boost_cur_ = 0.0;
  double shift_boost_peak_ = 0.0;
  double shift_boost_elapsed_s_ = 0.0;
  double shift_boost_dur_s_ = 0.0;

  // Focus _ET.xml DSP state (FocusPEQ, Distortion, 3-band TrashDSP).
  Biquad focus_peq_l_, focus_peq_r_;
  double cur_focus_peq_gain_ = 1.0;
  double cur_dist_level_ = 0.0;
  double cur_dist_comp_ = 1.0;
  double trash_lp1_[2]{0.0, 0.0};
  double trash_lp2_[2]{0.0, 0.0};

  // Optional pinned owner (e.g. BakedSound) released on main thread after audio-thread retirement.
  std::shared_ptr<const void> owner_;
};

}  // namespace audio
