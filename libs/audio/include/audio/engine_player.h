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
  // Starts the ShiftVolumeScalar envelope (upshift or downshift): a factor
  // on the focus Trash bands' output gain in dB that falls back to 1 (Q322).
  void trigger_shift_boost(bool upshift);
  // Advances Expander & shift-boost timers by `dt` seconds (also advanced
  // automatically by render()).
  void advance_dsp(double dt);

  // Mixes `frames` stereo frames (interleaved L, R) into `out` (overwrites).
  void render(float* out, int frames);

  bool neg_raises = true;  // confirmed SPEC-ENGINE-SOUND: braking raises x
  double master = 1;       // overall level
  // The FocusCar category gain of the snapshot matrix (Q337). The original
  // applies it to the focus voices ahead of the chain's shaper: halving it
  // lowers the idle 6 dB and the saturated limiter only about 3 dB. Without a
  // matrix (an engine-only render) it stays at FreeRoam's 0.35.
  double category_gain = kFreeRoamCategoryGain;
  // Set when the caller's mixer multiplies this player's output by the same
  // category gain after it (the live mixer's engine bus): the chain's output
  // then divides it back out, so the gain counts once, before the shaper.
  bool category_applied_after = false;
  double pitch_scale = 1;  // multiplies each loop's rate on the next set() (a 3D car's Doppler shift)
  // A 3D point source (another car, at the Default view): each group plays at
  // unity in both channels and the caller places the sum; the 5.1 speaker mix
  // and the focus-car exhaust shaping (FocusPEQ, Distortion, TrashDSP) are the
  // focus car's alone (SPEC-ENGINE-SOUND, Mix basics and Focus DSP).
  bool positional = false;

  double audio_rpm() const { return audio_rpm_; }
  int voices() const;
  double expander_state(size_t group) const;
  double peq_gain(size_t group) const;
  double load_peq_gain(size_t group) const;
  double focus_peq_gain() const { return cur_focus_peq_gain_; }
  double distortion_level() const { return cur_dist_level_; }
  double shift_factor() const { return shift_factor_; }
  // The DSPScalar s (DSPMaxScalar x the car descriptor's value, Q322).
  double dsp_scalar() const;
  // The focus engine group's volume after the Trash DSP (1 while it is off).
  double trash_group_gain() const { return cur_trash_group_gain_; }
  // The car descriptor's scalar behind s (Q322, decoded in first30k): the
  // fitted intake, exhaust and camshaft levels over their stock rows' levels,
  // each over 3, weighted 0.165, 0.67 and 0.165, clamped to 0..1. The career
  // MX-5 (intake 3, exhaust 3, camshaft 1 over stock 0) gives 0.89, which the
  // original's captures had fitted as 0.9.
  static double descriptor_dsp_scalar(int intake_up, int exhaust_up, int camshaft_up) {
    const double v = (0.165 * intake_up + 0.67 * exhaust_up + 0.165 * camshaft_up) / 3.0;
    return v < 0 ? 0.0 : v > 1 ? 1.0 : v;
  }
  // The value used when the caller does not know the fitted parts.
  static constexpr double kDescriptorDspScalar = 0.9;
  double descriptor_scalar = kDescriptorDspScalar;
  // Measured against the original (Q322): the group volume per unit of
  // VolumeCompensate (0.175 x the audio manager's global level), relative to
  // the group volume with Trash off.
  static constexpr double kTrashGroupScale = 0.24;
  // The focus car's level into and out of its DSP chain, against the
  // original's captures (Q322, Q337). Into the chain the voices carry the
  // FocusCar category gain (`category_gain`) times kFocusPreScale: Q322 fitted
  // 0.1 (-20 dB) at FreeRoam's 0.35, so 0.1 / 0.35 per unit of category gain.
  static constexpr double kFreeRoamCategoryGain = 0.35;
  static constexpr double kFocusPreScale = 0.1 / kFreeRoamCategoryGain;
  static constexpr double kFocusPostScale = 2.24;   // +7 dB
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
    Biquad lp, lp2, peq, load_peq;
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

  // ShiftVolumeScalar envelope: the factor goes linearly from its start back to 1.
  double shift_factor_ = 1.0;
  double shift_start_ = 1.0;
  double shift_elapsed_s_ = 0.0;
  double shift_dur_s_ = 0.0;

  // Focus _ET.xml DSP state (FocusPEQ, Distortion, 3-band TrashDSP).
  Biquad focus_peq_l_, focus_peq_r_;
  double cur_focus_peq_gain_ = 1.0;
  double cur_dist_level_ = 0.0;
  // Trash: the bands' values for this update, and per channel the states of the
  // first-order sections LP(c1), HP(c1) then LP(c2), and HP(c2).
  struct TrashBandNow {
    int type = 0;
    double in = 1, drive = 1, mix = 0, out_db = 0;
  };
  std::array<TrashBandNow, 3> trash_now_{};
  double cur_trash_volcomp_ = 1.0;
  double cur_trash_group_gain_ = 1.0;
  struct OnePole {
    double b0 = 1, b1 = 0, a1 = 0, x1 = 0, y1 = 0;
    double tick(double x) {
      const double y = b0 * x + b1 * x1 - a1 * y1;
      x1 = x;
      y1 = std::isfinite(y) ? y : 0.0;
      if (std::fabs(y1) < 1e-20) y1 = 0.0;
      return y1;
    }
  };
  std::array<OnePole, 2> trash_lp1_{}, trash_hp1_{}, trash_lp2_{}, trash_hp2_{};
  bool trash_c1_on_ = true, trash_c2_on_ = true;
  double cur_master_ = 1.0;

  // Optional pinned owner (e.g. BakedSound) released on main thread after audio-thread retirement.
  std::shared_ptr<const void> owner_;
};

}  // namespace audio
