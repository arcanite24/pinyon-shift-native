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
  // FMOD Ex's ParamEQ takes the gain as the RBJ A itself, so the peak is
  // gain^2 (2.3 is +14.5 dB): the original's captures need it (Q322).
  const double A = linear_gain;
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

void EnginePlayer::trigger_shift_boost(bool upshift) {
  const auto& svs = sound_.shift_volume_scalar;
  if (!svs.present) return;
  // The game's ShiftVolumeScalar (Q322): Up/DownPct is the starting factor on
  // the Trash bands' output gain in dB, falling linearly to 1 over Up/DownTime.
  const double start = upshift ? svs.boost_up_pct : svs.boost_down_pct;
  const double dur = upshift ? svs.boost_up_time_s : svs.boost_down_time_s;
  if (std::fabs(start - 1.0) <= 1e-4 || dur <= 1e-3) return;
  shift_start_ = start;
  shift_dur_s_ = dur;
  shift_elapsed_s_ = 0.0;
  shift_factor_ = start;
}

double EnginePlayer::dsp_scalar() const {
  const auto& d = sound_.dsp_scalar;
  return d.present ? d.dsp_max_scalar * kDescriptorDspScalar : 0.0;
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
  dsp_time_s_ += clamped_dt;
  const double dt_ms = clamped_dt * 1000.0;
  for (size_t g = 0; g < state_.size(); ++g) update_expander_state(g, dt_ms);

  // ShiftVolumeScalar envelope.
  if (shift_dur_s_ > 0.0) {
    shift_elapsed_s_ += clamped_dt;
    if (shift_elapsed_s_ >= shift_dur_s_) {
      shift_factor_ = 1.0;
      shift_dur_s_ = 0.0;
    } else {
      shift_factor_ = shift_start_ + (1.0 - shift_start_) * (shift_elapsed_s_ / shift_dur_s_);
    }
  } else {
    shift_factor_ = 1.0;
  }

  // No drivetrain_spring stage. The engine tuning's drivetrain_spring_*
  // values are parsed (engine_sound.cpp), but the game's two-spring update
  // is not decoded, and SPEC-ENGINE-SOUND "Unknowns" says to omit it.
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
  SoundInputs in{n, thr, pos_t, neg_in};
  // The loops reach the focus DSP at their own level: `master` is applied
  // after it, so the Trash drive does not move with the output level.
  const double global = safe_clamp(sound_.global_volume.eval(in, neg_raises) * sound_.engine_volume, 0.0, 16.0, 0.0);
  cur_master_ = safe_clamp(master, 0.0, 16.0, 0.0);
  const size_t view = size_t(std::clamp(int(u.view), 0, kViews - 1));
  const double ds = dsp_scalar();
  const DspScalarConfig& dsc = sound_.dsp_scalar;

  // Focus _ET.xml DSP. FocusPEQ, with the DSPScalar offsets on its curves
  // (game clamps: gain 0.05..3, centre 20..22000 Hz, bandwidth 0.2..5).
  if (sound_.focus_peq.active && !positional) {
    const double fg =
        safe_clamp(sound_.focus_peq.gain.eval(in, neg_raises) + dsc.peq_gain_max * ds, 0.05, 3.0, 1.0);
    const double ffc =
        safe_clamp(sound_.focus_peq.center_freq.eval(in, neg_raises) + dsc.peq_freq_max * ds, 20.0, 22000.0, 1000.0);
    const double fbw =
        safe_clamp(sound_.focus_peq.bandwidth.eval(in, neg_raises) + dsc.peq_bandwidth_max * ds, 0.2, 5.0, 1.0);
    cur_focus_peq_gain_ = fg;
    focus_peq_l_.set_peq(std::min(ffc, 0.45 * rate_), fbw, fg, rate_);
    focus_peq_r_.set_peq(std::min(ffc, 0.45 * rate_), fbw, fg, rate_);
  } else {
    cur_focus_peq_gain_ = 1.0;
    focus_peq_l_.set_bypass();
    focus_peq_r_.set_bypass();
  }
  // The FMOD Distortion is the other cars' alternative to Trash; the focus car
  // never runs it (the nodist probe, Q322), and the other cars' use is not yet
  // modelled.
  cur_dist_level_ = 0.0;
  // Trash: the band values the game sends this update (Q321).
  if (sound_.trash_dsp.active && !positional) {
    const TrashDspConfig& td = sound_.trash_dsp;
    for (size_t b = 0; b < 3; ++b) {
      const TrashBandConfig& c = td.bands[b];
      double in_db = c.input_gain_db, od = c.overdrive, mix = c.mix * 100.0, out_db = c.output_gain_db;
      if (td.use_curves) {
        if (c.input_gain_curve.present) in_db = c.input_gain_curve.eval(in, neg_raises);
        if (c.overdrive_curve.present) od = c.overdrive_curve.eval(in, neg_raises);
        if (c.mix_curve.present) mix = c.mix_curve.eval(in, neg_raises);
        if (c.output_gain_curve.present) out_db = c.output_gain_curve.eval(in, neg_raises);
      }
      od = safe_clamp(od + dsc.trash_od_max[b] * ds, 0.0, 10.0, 0.0);
      TrashBandNow& bn = trash_now_[b];
      bn.type = c.effect_type;
      bn.in = std::pow(10.0, safe_clamp(in_db, -30.0, 20.0, 0.0) / 20.0);
      bn.drive = std::pow(10.0, 4.0 * od / 20.0);
      bn.mix = safe_clamp(mix, 0.0, 100.0, 0.0) * 0.01;
      bn.out_db = safe_clamp(out_db, -40.0, 10.0, 0.0);
    }
    cur_trash_volcomp_ = safe_clamp(td.volume_compensate + dsc.trash_volcomp_min * ds, 0.0, 1.0, 1.0);
    cur_trash_group_gain_ = cur_trash_volcomp_ * kTrashGroupScale;
    // First-order bilinear sections (prewarped); a cutoff at or above
    // Nyquist leaves its split out.
    const double nyq = 0.5 * rate_;
    trash_c1_on_ = td.cutoff1_hz > 0.0 && td.cutoff1_hz < nyq;
    trash_c2_on_ = td.cutoff2_hz > 0.0 && td.cutoff2_hz < nyq;
    auto set_pair = [&](double fc, OnePole& lp, OnePole& hp) {
      const double k = std::tan(kPi * std::min(fc, 0.499 * rate_) / rate_);
      const double a1 = (k - 1.0) / (k + 1.0);
      lp.b0 = lp.b1 = k / (1.0 + k);
      lp.a1 = a1;
      hp.b0 = 1.0 / (1.0 + k);
      hp.b1 = -hp.b0;
      hp.a1 = a1;
    };
    for (int c = 0; c < 2; ++c) {
      set_pair(std::max(td.cutoff1_hz, 1.0), trash_lp1_[size_t(c)], trash_hp1_[size_t(c)]);
      set_pair(std::max(td.cutoff2_hz, 1.0), trash_lp2_[size_t(c)], trash_hp2_[size_t(c)]);
    }
  } else {
    cur_trash_volcomp_ = 1.0;
    cur_trash_group_gain_ = 1.0;
  }

  // IntakeChuff one-shot trigger (SPEC-ENGINE-SOUND): snap-open from idle
  // evaluated over a 33 ms sliding throttle history window so high-FPS (120/240 Hz)
  // throttle ramps accumulate reliably instead of failing a 1-frame delta test.
  double min_recent_thr = thr;
  bool have_window_sample = false;
  constexpr double kChuffWindowS = 0.0335;
  for (int i = 0; i < thr_hist_count_; ++i) {
    const int idx = (thr_hist_head_ - 1 - i + kThrottleHistSize) % kThrottleHistSize;
    const auto& entry = thr_hist_[size_t(idx)];
    const double age = dsp_time_s_ - entry.time_s;
    if (i == 0 || (age >= -1e-6 && age <= kChuffWindowS)) {
      min_recent_thr = std::min(min_recent_thr, entry.throttle);
      have_window_sample = true;
    }
  }
  if (thr <= 0.02) chuff_armed_ = true;
  if (chuff_pcm_ && !chuff_pcm_->data.empty() && have_window_sample && chuff_armed_ && !chuff_active_ &&
      min_recent_thr <= 0.02 && (thr - min_recent_thr) > 0.1 && n < 0.425) {
    double intake_view = 1.0, pl = 1.0, pr = 1.0;
    for (const auto& g : sound_.groups) {
      if (iequals(g.name, "EngineIntake")) {
        intake_view = g.view_level[view];
        pl = positional ? 1.0 : g.pan_left[view];
        pr = positional ? 1.0 : g.pan_right[view];
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
    if (chuff_active_) chuff_armed_ = false;
  }
  thr_hist_[size_t(thr_hist_head_)] = {dsp_time_s_, thr};
  thr_hist_head_ = (thr_hist_head_ + 1) % kThrottleHistSize;
  if (thr_hist_count_ < kThrottleHistSize) ++thr_hist_count_;

  for (size_t g = 0; g < state_.size(); ++g) {
    const Group& group = sound_.groups[g];
    GroupState& st = state_[g];
    st.exp_max_gain = group.expander.present ? std::max(0.0, group.expander.max_gain.eval(in, neg_raises)) : 0.0;
    st.base_target_gain = group.gain.eval(in, neg_raises) * group.view_level[view] * global;
    st.target_gain = st.base_target_gain * (1.0 + st.exp_state);
    st.left = positional ? 1.0 : group.pan_left[view];
    st.right = positional ? 1.0 : group.pan_right[view];

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
        v.target_step = safe_clamp(std::max(0.0, pitch_ratio(group.loops[i], audio_rpm_)) * pcm->rate / rate_ * pitch_scale, 0.0,
                                  16.0, 1.0);
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

  const bool run_focus_peq = sound_.focus_peq.active && !positional;
  const bool run_trash = sound_.trash_dsp.active && !positional;
  // Every focus car goes through the chain (its level in and out of it is
  // the focus car's, with or without a FocusPEQ or Trash on the tuning).
  const bool run_focus_dsp = !positional;
  // The focus chain carries every emission group (Q322): the loops are summed
  // at their own level, shaped, then set to the group volume and `master`.
  const double post_gain = cur_master_ * kFocusPostScale * (run_trash ? cur_trash_group_gain_ : 1.0);
  std::array<double, 3> out_gain{1, 1, 1};
  for (size_t b = 0; b < 3; ++b) out_gain[b] = std::pow(10.0, trash_now_[b].out_db * shift_factor_ / 20.0);
  auto shape_band = [&](double x, size_t b) {
    const TrashBandNow& n = trash_now_[b];
    const double y = x * n.in * n.drive;
    double wet = y;
    if (n.type == 1) {
      wet = y * (1.0 + std::fabs(y)) / (1.0 + y * y);
    } else if (n.type == 2) {
      const double ay = std::fabs(y);
      wet = ay >= 1.0 ? (y < 0 ? -1.0 : 1.0) : (y < 0 ? -(2.0 * ay - ay * ay) : (2.0 * ay - ay * ay));
    }
    const double dry = n.type == 1 ? x : x * n.in;
    return out_gain[b] * ((1.0 - n.mix) * dry + n.mix * wet);
  };

  // 256-frame stack chunks: the focus car's groups go through FocusPEQ and
  // Trash together; a positional car's groups go straight out.
  constexpr int kChunk = 256;
  float scoped_buf[kChunk * 2];
  for (int off = 0; off < frames; off += kChunk) {
    const int n_chunk = std::min(kChunk, frames - off);
    float* out_chunk = out + 2 * size_t(off);
    if (run_focus_dsp) std::fill(scoped_buf, scoped_buf + 2 * size_t(n_chunk), 0.0f);
    for (size_t g = 0; g < state_.size(); ++g) {
      GroupState& st = state_[g];
      float* dst = run_focus_dsp ? scoped_buf : out_chunk;
      const double group_post = run_focus_dsp ? kFocusPreScale : cur_master_;
      for (int f = 0; f < n_chunk; ++f) {
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
        s *= st.gain * group_post;
        if (std::isfinite(s)) {
          dst[2 * f] += float(s * st.left);
          dst[2 * f + 1] += float(s * st.right);
        }
      }
    }

    if (run_focus_dsp) {
      for (int f = 0; f < n_chunk; ++f) {
        double ch[2] = {double(scoped_buf[2 * f]), double(scoped_buf[2 * f + 1])};
        if (run_focus_peq) {
          ch[0] = focus_peq_l_.tick(ch[0], smooth_);
          ch[1] = focus_peq_r_.tick(ch[1], smooth_);
        }
        if (run_trash) {
          for (size_t c = 0; c < 2; ++c) {
            const double x = ch[c];
            const double lo = trash_c1_on_ ? trash_lp1_[c].tick(x) : x;
            double mid = trash_c1_on_ ? trash_hp1_[c].tick(x) : 0.0;
            if (trash_c1_on_ && trash_c2_on_) mid = trash_lp2_[c].tick(mid);
            const double hi = trash_c2_on_ ? trash_hp2_[c].tick(x) : 0.0;
            ch[c] = shape_band(lo, 0) + (trash_c1_on_ ? shape_band(mid, 1) : 0.0) +
                    (trash_c2_on_ ? shape_band(hi, 2) : 0.0);
          }
        }
        ch[0] *= post_gain;
        ch[1] *= post_gain;
        if (std::isfinite(ch[0])) out_chunk[2 * f] += float(ch[0]);
        if (std::isfinite(ch[1])) out_chunk[2 * f + 1] += float(ch[1]);
      }
    }
  }

  // Mix one-shot IntakeChuff if active (uncolored by exhaust Distortion/TrashDSP).
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
}

}  // namespace audio
