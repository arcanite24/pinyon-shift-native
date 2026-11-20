#include "audio/engine_player.h"

#include <algorithm>
#include <cmath>

namespace audio {

EnginePlayer::EnginePlayer(const EngineSound& sound, std::vector<std::vector<const PcmSample*>> pcm, int out_rate,
                           double idle, double redline)
    : sound_(sound), pcm_(std::move(pcm)), rate_(std::max(1, out_rate)), idle_(idle), redline_(redline) {
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

void EnginePlayer::set(const Update& u) {
  double n = 0;
  audio_rpm_ = audio::audio_rpm(u.rpm, idle_, redline_, sound_.audio_rpm_idle, sound_.audio_rpm_redline, &n);
  SoundInputs in{n, std::clamp(u.throttle, 0.0, 1.0), std::clamp(u.pos_torque, 0.0, 1.0),
                 std::clamp(u.neg_torque, 0.0, 1.0)};
  const double global = sound_.global_volume.eval(in, neg_raises) * sound_.engine_volume * master;
  const size_t view = size_t(u.view);
  for (size_t g = 0; g < state_.size(); ++g) {
    const Group& group = sound_.groups[g];
    GroupState& st = state_[g];
    st.target_gain = group.gain.eval(in, neg_raises) * group.view_level[view] * global;
    st.left = group.pan_left[view];
    st.right = group.pan_right[view];
    st.cutoff = group.lowpass ? std::clamp(group.lowpass_cutoff.eval(in, neg_raises), 20.0, 0.45 * rate_) : 0;
    const std::vector<double> fade = crossfade(group.loops, audio_rpm_);
    for (size_t i = 0; i < group.loops.size(); ++i) {
      Voice& v = st.voices[i];
      const PcmSample* pcm = pcm_[g][i];
      v.target_gain = pcm ? fade[i] * group.loops[i].volume : 0;
      if (pcm) v.target_step = std::max(0.0, pitch_ratio(group.loops[i], audio_rpm_)) * pcm->rate / rate_;
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
  std::fill(out, out + 2 * size_t(std::max(0, frames)), 0.0f);
  for (size_t g = 0; g < state_.size(); ++g) {
    GroupState& st = state_[g];
    const double lp_a = st.cutoff > 0 ? 1 - std::exp(-2 * 3.14159265358979 * st.cutoff / rate_) : 1;
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
        const size_t k = size_t(v.phase);
        const double t = v.phase - double(k);
        const float a = pcm->data[k % n], b = pcm->data[(k + 1) % n];
        sum += v.gain * (a + (b - a) * t);
        v.phase += v.step;
        if (v.phase >= double(n)) v.phase = std::fmod(v.phase, double(n));
      }
      st.gain += (st.target_gain - st.gain) * smooth_;
      st.lp += (sum - st.lp) * lp_a;
      const double s = st.lp * st.gain;
      out[2 * f] += float(s * st.left);
      out[2 * f + 1] += float(s * st.right);
    }
  }
}

}  // namespace audio
