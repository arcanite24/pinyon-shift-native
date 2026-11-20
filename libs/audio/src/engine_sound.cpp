#include "audio/engine_sound.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace audio {

double Param::eval(const SoundInputs& in, bool neg_raises) const {
  if (!present) return 1;
  const double neg = neg_raises ? in.neg_torque : -in.neg_torque;
  const double v = rpm * in.n + throttle * in.throttle + pos_torque * in.pos_torque + neg_torque * neg;
  if (v <= x[0]) return y[0];
  if (v >= x[2]) return y[2];
  const int k = v <= x[1] ? 0 : 1;
  const double span = x[k + 1] - x[k];
  if (span <= 0) return y[k + 1];
  return y[k] + (y[k + 1] - y[k]) * (v - x[k]) / span;
}

double audio_rpm(double rpm, double idle, double redline, double audio_idle, double audio_redline, double* n) {
  const double span = redline - idle;
  const double nn = span != 0 ? (rpm - idle) / span : 0;
  if (n) *n = nn;
  return audio_idle + (audio_redline - audio_idle) * nn;
}

std::vector<double> crossfade(const std::vector<Loop>& loops, double r) {
  std::vector<double> gains(loops.size(), 0.0);
  for (size_t i = 0; i < loops.size(); ++i) {
    const Loop& l = loops[i];
    if (r < l.rpm_min || r > l.rpm_max) continue;
    // The missing neighbour's edges are 0 and 32,000 (spec).
    const double prev_max = i > 0 ? loops[i - 1].rpm_max : 0.0;
    const double next_min = i + 1 < loops.size() ? loops[i + 1].rpm_min : 32000.0;
    double t = 1;
    if (i > 0 && r < prev_max) {
      const double span = prev_max - l.rpm_min;
      t = span > 0 ? (r - l.rpm_min) / span : 1;
    } else if (i + 1 < loops.size() && r > next_min) {
      const double span = l.rpm_max - next_min;
      t = span > 0 ? (l.rpm_max - r) / span : 1;
    }
    t = std::clamp(t, 0.0, 1.0);
    gains[i] = t >= 1 ? 1.0 : std::log2(1 + t);
  }
  return gains;
}

double pitch_ratio(const Loop& l, double r) {
  const double s = std::max(1, l.rpm_sample);
  const double slope = r <= s ? l.pitch_min : l.pitch_max;
  return l.pitch + slope * (r - s) / s;
}

void resolve_loop_indices(std::vector<Loop>& loops, int n, const std::vector<int>& channels) {
  for (Loop& l : loops) {
    const int i = std::max(0, l.bank_index);
    if (n <= 0) {
      l.sample = 0, l.channel = 0;
      continue;
    }
    l.sample = i % n;
    const int ch = i / n;
    // Past a mono bank: the same mono sample (spec, 65 ExhaustR records).
    const int available = size_t(l.sample) < channels.size() ? channels[size_t(l.sample)] : 1;
    l.channel = ch < available ? ch : 0;
  }
}

bool silent_name(const std::string& name) {
  std::string s;
  for (char c : name) s += char(std::tolower(static_cast<unsigned char>(c)));
  const size_t slash = s.find_last_of("/\\");
  if (slash != std::string::npos) s = s.substr(slash + 1);
  return s.empty() || s == "na.xml" || s == "na.fsb" || (s.rfind("na_", 0) == 0 && s.size() > 7 &&
                                                         s.compare(s.size() - 7, 7, "_ht.xml") == 0);
}

namespace {

Param read_param(const XmlNode* node) {
  Param p;
  if (!node) return p;
  const XmlNode* coeff = node->child("PhysicsCoeff");
  const XmlNode* curve = node->child("ThreePointCurve");
  if (!coeff || !curve) return p;
  p.present = true;
  auto c01 = [](double v) { return std::clamp(v, 0.0, 1.0); };
  p.rpm = c01(coeff->number("RPM"));
  p.throttle = c01(coeff->number("Throttle"));
  p.pos_torque = c01(coeff->number("PosTorque"));
  p.neg_torque = c01(coeff->number("NegTorque"));
  for (int k = 0; k < 3; ++k) {
    const std::string i = std::to_string(k);
    p.x[size_t(k)] = c01(curve->number("x" + i, 0.5 * k));
    p.y[size_t(k)] = curve->number("y" + i, 1);
  }
  return p;
}

int group_number(const std::string& name) {
  if (iequals(name, "EngineIntake")) return 0;
  if (iequals(name, "EngineAmbient")) return 1;
  return 2;  // ExhaustL and ExhaustR share the exhaust's settings
}

}  // namespace

bool read_engine_tuning(const XmlNode& engine, EngineSound& out, std::vector<HarmonicRef>& harmonics,
                        std::string* error) {
  if (!iequals(engine.name, "Engine")) {
    if (error) *error = "not an engine tuning (root " + engine.name + ")";
    return false;
  }
  if (const XmlNode* s = engine.child("EngineSettings")) {
    out.audio_rpm_idle = std::min(32000.0, s->number("audio_rpm_idle", out.audio_rpm_idle));
    out.audio_rpm_redline = std::min(32000.0, s->number("audio_rpm_redline", out.audio_rpm_redline));
  }
  if (const XmlNode* v = engine.child("VolumeAdjustments")) out.engine_volume = v->number("EngineVolume", 1);
  out.global_volume = read_param(engine.child("GlobalCarVolume"));
  harmonics.clear();
  for (const char* element : {"EngineIntake", "EngineAmbient", "Exhaust"}) {
    const XmlNode* g = engine.child(element);
    const XmlNode* up = g ? g->child("Upgrade") : nullptr;
    const std::string file = up ? up->attr("L0") : std::string();  // stock: L0
    if (file.empty() || silent_name(file)) continue;
    harmonics.push_back({element, file});
  }
  return true;
}

bool read_harmonic(const XmlNode& engine, const XmlNode& harmonic, const std::string& element, EngineSound& out,
                   std::string* error) {
  if (!iequals(harmonic.name, "Soundbank")) {
    if (error) *error = "not a harmonic tuning (root " + harmonic.name + ")";
    return false;
  }
  const std::string bank = harmonic.attr("name");
  if (bank.empty() || silent_name(bank)) return true;
  std::vector<std::string> names;
  if (iequals(element, "Exhaust")) names = {"ExhaustL", "ExhaustR"};
  else names = {element};
  for (const auto& name : names) {
    const XmlNode* set = harmonic.child(name);
    if (!set) continue;
    Group g;
    g.name = name;
    g.bank = bank;
    for (const auto& l : set->children) {
      if (!iequals(l.name, "Loop") || g.loops.size() >= 16) continue;
      Loop loop;
      loop.bank_index = int(l.number("wavebankindex"));
      loop.rpm_min = int(std::clamp(l.number("rpm_min"), 0.0, 32000.0));
      loop.rpm_max = int(std::clamp(l.number("rpm_max"), 0.0, 32000.0));
      loop.rpm_sample = int(std::clamp(l.number("rpm_sample", 1), 1.0, 32000.0));
      loop.volume = l.number("volume", 1);
      loop.pitch = l.number("pitch", 1);
      loop.pitch_min = l.number("pitchMin", 1);
      loop.pitch_max = l.number("pitchMax", 1);
      g.loops.push_back(loop);
    }
    if (g.loops.empty()) continue;
    // The group gain: the engine tuning's EmissionGroup<g> when present
    // (probable: read after the harmonic tuning), else the harmonic's DSP.
    const std::string eg = "HarmonicTunings/EmissionGroup" + std::to_string(group_number(name));
    const XmlNode* engine_group = engine.find(eg);
    const XmlNode* dsp = harmonic.child("DSP");
    const XmlNode* gain = engine_group ? engine_group->find("Volume/Gain") : nullptr;
    if (!gain && dsp) gain = dsp->find("Volume/Gain");
    g.gain = read_param(gain);
    const XmlNode* lp = engine_group ? engine_group->child("Lowpass") : nullptr;
    if (!lp && dsp) lp = dsp->child("Lowpass");
    if (lp && lp->number("Active") >= 1) {
      g.lowpass = true;
      g.lowpass_cutoff = read_param(lp->child("CutoffFrequency"));
      g.lowpass = g.lowpass_cutoff.present;
    }
    out.groups.push_back(std::move(g));
  }
  return true;
}

void read_model_tuning(const XmlNode& model, EngineSound& out) {
  const XmlNode* basic = model.find("Listener/Basic");
  if (!basic) return;
  static const char* kViewNames[kViews] = {"Follow", "Cockpit", "Front", "Default"};
  for (int v = 0; v < kViews; ++v) {
    const XmlNode* view = basic->child(kViewNames[v]);
    if (!view) continue;
    const XmlNode* levels = view->child("EmissionGroupVolumes");
    for (Group& g : out.groups) {
      if (levels && levels->has(g.name)) g.view_level[size_t(v)] = levels->number(g.name, 1);
      // Our stereo fold of the 5.1 pan: L = Left + 0.7 Center + 0.7 LeftRear
      // and the mirror, scaled so the louder side is 1 (spec "Mix basics").
      if (const XmlNode* pan = view->child("SurroundPan" + g.name)) {
        const double c = pan->number("Center");
        double l = pan->number("Left") + 0.7 * c + 0.7 * pan->number("LeftRear");
        double r = pan->number("Right") + 0.7 * c + 0.7 * pan->number("RightRear");
        const double m = std::max(l, r);
        if (m > 0) l /= m, r /= m;
        g.pan_left[size_t(v)] = l;
        g.pan_right[size_t(v)] = r;
      }
    }
  }
}

}  // namespace audio
