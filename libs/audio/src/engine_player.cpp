#include "audio/engine_player.h"

#include <algorithm>
#include <cmath>

#include "audio/xml.h"

namespace audio {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kLn2 = 0.69314718055994530942;

double safe_finite(double v, double fallback) { return std::isfinite(v) ? v : fallback; }
double safe_clamp(double v, double lo, double hi, double fallback) {
  if (!std::isfinite(v)) return fallback;
  return std::clamp(v, lo, hi);
}
}  // namespace

void EnginePlayer::Biquad::set_lowpass(double fc, double q, int rate) {
  const double sr = double(std::max(8000, rate));
  fc = safe_clamp(fc, 20.0, 0.45 * sr, 0.45 * sr);
  q = safe_clamp(q, 0.1, 12.0, 0.707);
  const double w0 = 2.0 * kPi * fc / sr;
  const double cos_w0 = std::cos(w0);
  const double alpha = std::sin(w0) / (2.0 * q);
  const double a0 = 1.0 + alpha;
  if (!(a0 > 1e-9) || !std::isfinite(a0)) {
    set_bypass();
    return;
  }
  tb0 = ((1.0 - cos_w0) * 0.5) / a0;
  tb1 = (1.0 - cos_w0) / a0;
  tb2 = ((1.0 - cos_w0) * 0.5) / a0;
  ta1 = (-2.0 * cos_w0) / a0;
  ta2 = (1.0 - alpha) / a0;
  if (!std::isfinite(tb0) || !std::isfinite(tb1) || !std::isfinite(tb2) || !std::isfinite(ta1) || !std::isfinite(ta2)) {
    set_bypass();
    return;
  }
  enabled = true;
}

void EnginePlayer::Biquad::set_peq(double fc, double bw_oct, double linear_gain, int rate) {
  const double sr = double(std::max(8000, rate));
  fc = safe_clamp(fc, 20.0, 0.45 * sr, 1000.0);
  bw_oct = safe_clamp(bw_oct, 0.1, 6.0, 1.0);
  linear_gain = safe_clamp(linear_gain, 0.05, 3.0, 1.0);
  if (std::fabs(linear_gain - 1.0) < 1e-4) {
    set_bypass();
    return;
  }
  const double A = std::sqrt(linear_gain);
  const double w0 = 2.0 * kPi * fc / sr;
  const double sin_w0 = std::sin(w0);
  const double cos_w0 = std::cos(w0);
  const double alpha = sin_w0 > 1e-6 ? sin_w0 * std::sinh((kLn2 * 0.5) * bw_oct * (w0 / sin_w0)) : 1e-6;
  const double a0 = 1.0 + alpha / A;
  if (!(a0 > 1e-9) || !std::isfinite(a0)) {
    set_bypass();
    return;
  }
  tb0 = (1.0 + alpha * A) / a0;
  tb1 = (-2.0 * cos_w0) / a0;
  tb2 = (1.0 - alpha * A) / a0;
  ta1 = (-2.0 * cos_w0) / a0;
  ta2 = (1.0 - alpha / A) / a0;
  if (!std::isfinite(tb0) || !std::isfinite(tb1) || !std::isfinite(tb2) || !std::isfinite(ta1) || !std::isfinite(ta2)) {
    set_bypass();
    return;
  }
  enabled = true;
}

EnginePlayer::EnginePlayer(const EngineSound& sound, std::vector<std::vector<const PcmSample*>> pcm, int out_rate,
                           double idle, double redline, const PcmSample* intake_chuff)
    : sound_(sound),
      pcm_(std::move(pcm)),
      rate_(std::max(1, out_rate)),
      idle_(idle),
      redline_(redline),
      chuff_pcm_(intake_chuff) {
  state_.resize(sound_.groups.size());
  for (size_t g = 0; g < state_.size(); ++g) {
    state_[g].voices.resize(sound_.groups[g].loops.size());
    if (pcm_.size() <= g) pcm_.resize(g + 1);
    pcm_[g].resize(sound_.groups[g].loops.size(), nullptr);
  }
  smooth_ = 1 - std::exp(-1.0 / (0.005 * rate_));
  // The game multiplies a voice that left its range by 0.2 per audio update
  // (about 30 a second): per output frame, 0.2^(30 / rate).
  fade_ = std::pow(0.2, 30.0 / rate_);
}

int EnginePlayer::voices() const {
  int n = 0;
  for (const auto& s : state_)
    for (const auto& v : s.voices) n += v.active;
  return n;
}

double EnginePlayer::expander_state(size_t group) const {
  return group < state_.size() ? state_[group].exp_state : 0.0;
}

double EnginePlayer::peq_gain(size_t group) const {
  return group < state_.size() ? state_[group].cur_peq_gain : 1.0;
}

double EnginePlayer::load_peq_gain(size_t group) const {
  return group < state_.size() ? state_[group].cur_load_peq_gain : 1.0;
}

void EnginePlayer::trigger_expander(int group) {
  for (size_t g = 0; g < state_.size(); ++g) {
    if (group >= 0 && int(g) != group) continue;
    const Group& grp = sound_.groups[g];
    if (group < 0 && !iequals(grp.name, "ExhaustL") && !iequals(grp.name, "ExhaustR")) continue;
    if (!grp.expander.present) continue;
    state_[g].exp_triggered = true;
    state_[g].exp_elapsed_ms = 0.0;
    state_[g].exp_state = 0.0;
  }
}

void EnginePlayer::update_expander_state(size_t g, double dt_ms) {
  GroupState& st = state_[g];
  const ExpanderConfig& exp = sound_.groups[g].expander;
  if (!st.exp_triggered || !exp.present) {
    st.exp_state = 0.0;
    return;
  }
  st.exp_elapsed_ms += dt_ms;
  const double att = exp.attack_ms;
  const double hold_end = att + exp.hold_ms;
  const double rel_end = hold_end + exp.release_ms;
  if (st.exp_elapsed_ms < att) {
    st.exp_state = 0.0;
  } else if (st.exp_elapsed_ms < hold_end) {
    st.exp_state = std::max(0.0, st.exp_max_gain);
  } else if (st.exp_elapsed_ms < rel_end) {
    st.exp_state = std::clamp(st.exp_state, 0.0, std::max(0.0, st.exp_max_gain));
  } else {
    st.exp_state = 0.0;
    st.exp_triggered = false;
  }
  st.target_gain = st.base_target_gain * (1.0 + st.exp_state);
}

void EnginePlayer::advance_dsp(double dt) {
  if (!std::isfinite(dt) || dt <= 0.0) return;
  const double clamped_dt = std::min(dt, 10.0);
  const double dt_ms = clamped_dt * 1000.0;
  for (size_t g = 0; g < state_.size(); ++g) update_expander_state(g, dt_ms);

  // Two-mode damped drivetrain spring oscillation (drivetrain_spring_*).
  if (sound_.drivetrain_spring.enabled && sound_.drivetrain_spring.clamp > 0.0) {
    const auto& ds = sound_.drivetrain_spring;
    const double w1 = 2.0 * kPi * std::clamp(ds.spring_f1, 0.5, 45.0);
    const double w2 = 2.0 * kPi * std::clamp(ds.spring_f2, 0.5, 60.0);
    const double c1 = 2.0 * std::clamp(ds.damper_r1, 0.05, 10.0) * w1;
    const double c2 = 2.0 * std::clamp(ds.damper_r2, 0.05, 10.0) * w2;
    const int steps = std::max(1, std::min(64, int(std::ceil(clamped_dt / 0.002))));
    const double h = clamped_dt / double(steps);
    for (int s = 0; s < steps; ++s) {
      const double a1 = -w1 * w1 * spring_x1_ - c1 * spring_v1_;
      spring_v1_ += a1 * h;
      spring_x1_ += spring_v1_ * h;
      const double a2 = -w2 * w2 * spring_x2_ - c2 * spring_v2_;
      spring_v2_ += a2 * h;
      spring_x2_ += spring_v2_ * h;
    }
    if (!std::isfinite(spring_x1_) || !std::isfinite(spring_v1_)) spring_x1_ = spring_v1_ = 0.0;
    if (!std::isfinite(spring_x2_) || !std::isfinite(spring_v2_)) spring_x2_ = spring_v2_ = 0.0;
    const double lim = ds.clamp;
    spring_x1_ = std::clamp(spring_x1_, -lim, lim);
    spring_x2_ = std::clamp(spring_x2_, -lim, lim);
    rpm_spring_offset_ = std::clamp((spring_x1_ + 0.45 * spring_x2_) * ds.scalar, -lim, lim);
  } else {
    rpm_spring_offset_ = 0.0;
  }
}

void EnginePlayer::set(const Update& u) {
  double n = 0;
  const double safe_rpm = safe_clamp(u.rpm, 0.0, 32000.0, idle_);
  audio_rpm_ = audio::audio_rpm(safe_rpm, idle_, redline_, sound_.audio_rpm_idle, sound_.audio_rpm_redline, &n);
  if (!std::isfinite(audio_rpm_)) audio_rpm_ = sound_.audio_rpm_idle;
  n = safe_clamp(n, -0.5, 2.0, 0.0);
  const double thr = safe_clamp(u.throttle, 0.0, 1.0, 0.0);
  const double pos_t = safe_clamp(u.pos_torque, 0.0, 1.0, 0.0);
  const double raw_neg = safe_finite(u.neg_torque, 0.0);
  const double neg_in = raw_neg < 0 ? std::clamp(raw_neg, -1.0, 0.0) : std::clamp(raw_neg, 0.0, 1.0);
  const double signed_torque = pos_t - std::fabs(neg_in);
  if (sound_.drivetrain_spring.enabled && sound_.drivetrain_spring.clamp > 0.0) {
    if (have_spring_torque_) {
      const double dtq = std::clamp(signed_torque - prev_spring_torque_, -2.0, 2.0);
      if (std::fabs(dtq) > 0.02) {
        const double impulse = dtq * std::min(sound_.drivetrain_spring.clamp, 600.0) * 6.0;
        spring_v1_ += impulse;
        spring_v2_ += impulse * 0.6;
      }
    }
    prev_spring_torque_ = signed_torque;
    have_spring_torque_ = true;
    audio_rpm_ = std::clamp(audio_rpm_ + rpm_spring_offset_, 0.0, 32000.0);
  }
  SoundInputs in{n, thr, pos_t, neg_in};
  const double global = safe_clamp(sound_.global_volume.eval(in, neg_raises) * sound_.engine_volume * master, 0.0, 16.0, 0.0);
  const size_t view = size_t(std::clamp(int(u.view), 0, kViews - 1));

  // Focus _ET.xml DSP: FocusPEQ & Distortion parameter updates.
  if (sound_.focus_peq.active) {
    const double fg = safe_clamp(sound_.focus_peq.gain.eval(in, neg_raises), 0.05, 3.0, 1.0);
    const double ffc = sound_.focus_peq.center_freq.eval(in, neg_raises);
    const double fbw = sound_.focus_peq.bandwidth.eval(in, neg_raises);
    cur_focus_peq_gain_ = fg;
    focus_peq_l_.set_peq(ffc, fbw, fg, rate_);
    focus_peq_r_.set_peq(ffc, fbw, fg, rate_);
  } else {
    cur_focus_peq_gain_ = 1.0;
    focus_peq_l_.set_bypass();
    focus_peq_r_.set_bypass();
  }
  if (sound_.distortion.active) {
    const double lv = sound_.distortion.level.present
                          ? sound_.distortion.level.eval(in, neg_raises)
                          : sound_.distortion.level_scalar;
    cur_dist_level_ = safe_clamp(lv, 0.0, 0.95, 0.0);
    cur_dist_comp_ = safe_clamp(sound_.distortion.volume_compensate, 0.1, 3.0, 1.0);
  } else {
    cur_dist_level_ = 0.0;
    cur_dist_comp_ = 1.0;
  }

  // IntakeChuff one-shot trigger (SPEC-ENGINE-SOUND): snap-open from idle
  // (prev_throttle == 0, !playing, delta > 0.1, n < 0.425).
  if (chuff_pcm_ && !chuff_pcm_->data.empty() && have_prev_throttle_ && !chuff_active_ && prev_throttle_ <= 0.001 &&
      (thr - prev_throttle_) > 0.1 && n < 0.425) {
    double intake_view = 1.0, pl = 1.0, pr = 1.0;
    for (const auto& g : sound_.groups) {
      if (iequals(g.name, "EngineIntake")) {
        intake_view = g.view_level[view];
        pl = g.pan_left[view];
        pr = g.pan_right[view];
        break;
      }
    }
    const double chuff_vol =
        sound_.intake_chuff_volume.present ? sound_.intake_chuff_volume.eval(in, neg_raises) : 0.2;
    chuff_gain_ = chuff_vol * intake_view * sound_.global_volume.eval(in, neg_raises) * master;
    chuff_left_ = pl;
    chuff_right_ = pr;
    chuff_phase_ = 0.0;
    chuff_active_ = chuff_gain_ > 1e-4;
  }
  prev_throttle_ = thr;
  have_prev_throttle_ = true;

  for (size_t g = 0; g < state_.size(); ++g) {
    const Group& group = sound_.groups[g];
    GroupState& st = state_[g];
    st.exp_max_gain = group.expander.present ? std::max(0.0, group.expander.max_gain.eval(in, neg_raises)) : 0.0;
    st.base_target_gain = group.gain.eval(in, neg_raises) * group.view_level[view] * global;
    st.target_gain = st.base_target_gain * (1.0 + st.exp_state);
    st.left = group.pan_left[view];
    st.right = group.pan_right[view];

    // 2nd-order resonant Lowpass.
    if (group.lowpass) {
      const double fc = safe_clamp(group.lowpass_cutoff.eval(in, neg_raises), 20.0, 0.45 * rate_, 0.45 * rate_);
      const double q =
          group.lowpass_resonance.present ? safe_clamp(group.lowpass_resonance.eval(in, neg_raises), 0.1, 12.0, 0.707) : 0.707;
      st.lp.set_lowpass(fc, q, rate_);
    } else {
      st.lp.set_bypass();
    }

    // PEQ with audio_rpm_idle_gain_reset linear blend on ExhaustL.
    if (group.peq.active) {
      double pg = safe_clamp(group.peq.gain.eval(in, neg_raises), 0.05, 3.0, 1.0);
      if (iequals(group.name, "ExhaustL") && group.idle_gain_reset > sound_.audio_rpm_idle + 1.0 &&
          audio_rpm_ < group.idle_gain_reset - 1.0) {
        const double span = (group.idle_gain_reset - 1.0) - sound_.audio_rpm_idle;
        const double t = span > 0 ? std::clamp((audio_rpm_ - sound_.audio_rpm_idle) / span, 0.0, 1.0) : 1.0;
        pg = 1.0 + (pg - 1.0) * t;
      }
      const double fc = group.peq.center_freq.eval(in, neg_raises);
      const double bw = group.peq.bandwidth.eval(in, neg_raises);
      st.cur_peq_gain = pg;
      st.peq.set_peq(fc, bw, pg, rate_);
    } else {
      st.cur_peq_gain = 1.0;
      st.peq.set_bypass();
    }

    // LoadPEQ: interpolate CenterFrequency, Bandwidth, Gain from NegLoad to PosLoad by throttle.
    if (group.load_peq.active) {
      const bool hp = group.load_peq.pos_load.active;
      const bool hn = group.load_peq.neg_load.active;
      const double pos_g = hp ? safe_finite(group.load_peq.pos_load.gain.eval(in, neg_raises), 1.0) : 1.0;
      const double neg_g = hn ? safe_finite(group.load_peq.neg_load.gain.eval(in, neg_raises), 1.0) : 1.0;
      const double pos_fc = hp ? safe_finite(group.load_peq.pos_load.center_freq.eval(in, neg_raises), 400.0) : 400.0;
      const double neg_fc = hn ? safe_finite(group.load_peq.neg_load.center_freq.eval(in, neg_raises), pos_fc) : pos_fc;
      const double pos_bw = hp ? safe_finite(group.load_peq.pos_load.bandwidth.eval(in, neg_raises), 1.0) : 1.0;
      const double neg_bw = hn ? safe_finite(group.load_peq.neg_load.bandwidth.eval(in, neg_raises), pos_bw) : pos_bw;
      const double lg = neg_g + (pos_g - neg_g) * thr;
      const double lfc = neg_fc + (pos_fc - neg_fc) * thr;
      const double lbw = neg_bw + (pos_bw - neg_bw) * thr;
      st.cur_load_peq_gain = lg;
      st.load_peq.set_peq(lfc, lbw, lg, rate_);
    } else {
      st.cur_load_peq_gain = 1.0;
      st.load_peq.set_bypass();
    }

    const std::vector<double> fade = crossfade(group.loops, audio_rpm_);
    for (size_t i = 0; i < group.loops.size(); ++i) {
      Voice& v = st.voices[i];
      const PcmSample* pcm = pcm_[g][i];
      v.target_gain = pcm ? safe_clamp(fade[i] * group.loops[i].volume, 0.0, 8.0, 0.0) : 0.0;
      if (pcm)
        v.target_step = safe_clamp(std::max(0.0, pitch_ratio(group.loops[i], audio_rpm_)) * pcm->rate / rate_, 0.0, 16.0, 1.0);
      if (!v.active && v.target_gain > 0.001) {
        v.active = true;
        v.phase = 0;
        v.gain = 0;
        v.step = v.target_step;
      }
    }
  }
}

void EnginePlayer::render(float* out, int frames) {
  if (frames <= 0) return;
  std::fill(out, out + 2 * size_t(frames), 0.0f);
  advance_dsp(double(frames) / double(rate_));
  for (size_t g = 0; g < state_.size(); ++g) {
    GroupState& st = state_[g];
    for (int f = 0; f < frames; ++f) {
      double sum = 0;
      for (size_t i = 0; i < st.voices.size(); ++i) {
        Voice& v = st.voices[i];
        if (!v.active) continue;
        const PcmSample* pcm = pcm_[g][i];
        const size_t n = pcm ? pcm->data.size() : 0;
        if (n == 0) {
          v.active = false;
          continue;
        }
        if (v.target_gain > 0) v.gain += (v.target_gain - v.gain) * smooth_;
        else if ((v.gain *= fade_) < 0.001) {
          v.active = false;
          continue;
        }
        v.step += (v.target_step - v.step) * smooth_;
        if (!std::isfinite(v.step) || v.step < 0.0) v.step = 0.0;
        if (!std::isfinite(v.phase) || v.phase < 0.0) v.phase = 0.0;
        const size_t k = size_t(v.phase);
        const double t = v.phase - double(k);
        const float a = pcm->data[k % n], b = pcm->data[(k + 1) % n];
        const double sample = double(a) + (double(b) - double(a)) * t;
        if (std::isfinite(sample) && std::isfinite(v.gain)) sum += v.gain * sample;
        v.phase += v.step;
        if (v.phase >= double(n)) v.phase = std::fmod(v.phase, double(n));
      }
      st.gain += (st.target_gain - st.gain) * smooth_;
      if (!std::isfinite(st.gain)) st.gain = 0.0;
      double s = st.peq.tick(sum, smooth_);
      s = st.load_peq.tick(s, smooth_);
      s = st.lp.tick(s, smooth_);
      s *= st.gain;
      if (std::isfinite(s)) {
        out[2 * f] += float(s * st.left);
        out[2 * f + 1] += float(s * st.right);
      }
    }
  }
  // Mix one-shot IntakeChuff if active.
  if (chuff_active_ && chuff_pcm_ && !chuff_pcm_->data.empty()) {
    const double step = double(std::max(1, chuff_pcm_->rate)) / double(rate_);
    const size_t n = chuff_pcm_->data.size();
    for (int f = 0; f < frames; ++f) {
      const size_t k = size_t(chuff_phase_);
      if (k >= n) {
        chuff_active_ = false;
        break;
      }
      const double t = chuff_phase_ - double(k);
      const float a = chuff_pcm_->data[k];
      const float b = (k + 1 < n) ? chuff_pcm_->data[k + 1] : 0.0f;
      const double s = (a + (b - a) * t) * chuff_gain_;
      out[2 * f] += float(s * chuff_left_);
      out[2 * f + 1] += float(s * chuff_right_);
      chuff_phase_ += step;
    }
  }
  // Apply Focus _ET.xml master engine DSP (FocusPEQ, Distortion, 3-band TrashDSP).
  const bool run_focus_peq = sound_.focus_peq.active;
  const bool run_dist = sound_.distortion.active && cur_dist_level_ > 1e-4;
  const bool run_trash = sound_.trash_dsp.active;
  if (run_focus_peq || run_dist || run_trash) {
    const double d_drive = 1.0 + 3.2 * cur_dist_level_;
    const double d_norm = d_drive > 1.0 ? (1.0 / std::tanh(d_drive)) : 1.0;
    const double a1 =
        run_trash ? (1.0 - std::exp(-2.0 * kPi * std::clamp(sound_.trash_dsp.cutoff1_hz, 40.0, 0.4 * rate_) / rate_))
                  : 0.0;
    const double a2 =
        run_trash ? (1.0 - std::exp(-2.0 * kPi * std::clamp(sound_.trash_dsp.cutoff2_hz, 80.0, 0.45 * rate_) / rate_))
                  : 0.0;
    auto shape_band = [](double x, const TrashBandConfig& b) {
      if (b.mix <= 1e-4 || b.overdrive <= 1e-4) return x * b.output_gain;
      const double k = 1.0 + 4.0 * b.overdrive;
      const double wet = std::tanh(x * b.input_gain * k) / std::tanh(k);
      return (x + (wet - x) * b.mix) * b.output_gain;
    };
    for (int f = 0; f < frames; ++f) {
      double ch[2] = {double(out[2 * f]), double(out[2 * f + 1])};
      if (run_focus_peq) {
        ch[0] = focus_peq_l_.tick(ch[0], smooth_);
        ch[1] = focus_peq_r_.tick(ch[1], smooth_);
      }
      if (run_dist) {
        for (int c = 0; c < 2; ++c) {
          const double wet = std::tanh(ch[c] * d_drive) * d_norm;
          ch[c] = (ch[c] + (wet - ch[c]) * cur_dist_level_) * cur_dist_comp_;
        }
      }
      if (run_trash) {
        for (int c = 0; c < 2; ++c) {
          trash_lp1_[c] += (ch[c] - trash_lp1_[c]) * a1;
          trash_lp2_[c] += (ch[c] - trash_lp2_[c]) * a2;
          const double lo = trash_lp1_[c];
          const double mid = trash_lp2_[c] - trash_lp1_[c];
          const double hi = ch[c] - trash_lp2_[c];
          ch[c] = shape_band(lo, sound_.trash_dsp.bands[0]) + shape_band(mid, sound_.trash_dsp.bands[1]) +
                  shape_band(hi, sound_.trash_dsp.bands[2]);
        }
      }
      out[2 * f] = std::isfinite(ch[0]) ? float(ch[0]) : 0.0f;
      out[2 * f + 1] = std::isfinite(ch[1]) ? float(ch[1]) : 0.0f;
    }
  }
}

}  // namespace audio
