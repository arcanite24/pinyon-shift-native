#include "audio/vo_player.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

#include "audio/fev.h"
#include "audio/xma_decode.h"
#include "audio/xml.h"
#include "cars/mods.h"

namespace audio {
namespace {

constexpr float kMonoPanGain = 0.70710678f;  // -3 dB equal-power center pan for mono VO

uint64_t fnv1a_64(std::string_view s) {
  if (s.empty()) return 0;
  uint64_t h = 14695981039346656037ull;
  for (unsigned char c : s) {
    h ^= uint64_t(c);
    h *= 1099511628211ull;
  }
  return h;
}

std::string lower_str(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string upper_str(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = char(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

std::string read_text(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return {};
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

bool read_fsb_headers(const std::filesystem::path& p, std::vector<FsbSample>& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  uint8_t head[48];
  if (!f.read(reinterpret_cast<char*>(head), 48)) return false;
  const uint32_t header_bytes = uint32_t(head[8]) | uint32_t(head[9]) << 8 | uint32_t(head[10]) << 16 |
                                uint32_t(head[11]) << 24;
  std::vector<uint8_t> bytes(48 + size_t(header_bytes));
  std::memcpy(bytes.data(), head, 48);
  if (!f.read(reinterpret_cast<char*>(bytes.data() + 48), header_bytes)) return false;
  std::error_code ec;
  const uint64_t size = std::filesystem::file_size(p, ec);
  return !ec && read_fsb4(bytes, out, nullptr, size);
}

// Lightweight PcmSource for XMA2 FSB4 subsounds (used by Rally_VO XMA2 banks
// inside StreamPool's background worker thread, never blocking the audio thread).
class FsbXmaSource final : public PcmSource {
 public:
  FsbXmaSource(std::filesystem::path bank, uint32_t subsound) : bank_(std::move(bank)), sub_(subsound) {}
  bool open() override {
    if (!decode_fsb_subsound_stereo(bank_, sub_, stereo_, rate_, {}, {})) return false;
    pos_ = 0;
    return !stereo_.empty() && rate_ > 0;
  }
  int rate() const override { return rate_; }
  int64_t length() const override { return int64_t(stereo_.size() / 2); }
  bool seek(int64_t frame) override {
    pos_ = std::clamp<int64_t>(frame, 0, length());
    return true;
  }
  int64_t read(float* out, int64_t frames) override {
    if (!out || frames <= 0) return 0;
    const int64_t avail = length() - pos_;
    const int64_t take = std::min(frames, avail);
    if (take <= 0) return 0;
    std::memcpy(out, stereo_.data() + size_t(pos_) * 2, size_t(take) * 2 * sizeof(float));
    pos_ += take;
    return take;
  }

 private:
  std::filesystem::path bank_;
  uint32_t sub_ = 0;
  std::vector<float> stereo_;
  int rate_ = 48000;
  int64_t pos_ = 0;
};

bool dir_has_rally_vo(const std::filesystem::path& dir) {
  std::error_code ec;
  if (dir.empty() || !std::filesystem::is_directory(dir, ec)) return false;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    if (!e.is_regular_file(ec)) continue;
    const std::string fn = lower_str(e.path().filename().string());
    if (fn.rfind("rally_vo", 0) == 0 || fn.rfind("rally_dialoguescript", 0) == 0) return true;
  }
  return false;
}

std::filesystem::path find_rally_vo_dir(const std::filesystem::path& root) {
  std::error_code ec;
  if (root.empty() || !std::filesystem::exists(root, ec)) return {};
  const std::filesystem::path direct[] = {
      root,
      root / "VO",
      root / "vo",
      root / "audio" / "VO",
      root / "audio" / "vo",
      root / "Media" / "audio" / "VO",
      root / "media" / "audio" / "vo",
      root / "Media" / "DLCZips" / "1600_pri_65" / "Media" / "audio" / "VO",
  };
  for (const auto& d : direct) {
    if (dir_has_rally_vo(d)) return d;
  }
  auto scan_dlczips = [&](const std::filesystem::path& dlczips) -> std::filesystem::path {
    if (!std::filesystem::is_directory(dlczips, ec)) return {};
    for (const auto& z : std::filesystem::directory_iterator(dlczips, ec)) {
      if (!z.is_directory(ec)) continue;
      for (const auto& sub : {z.path() / "Media" / "audio" / "VO", z.path() / "Media" / "audio" / "vo"}) {
        if (dir_has_rally_vo(sub)) return sub;
      }
    }
    return {};
  };
  if (auto hit = scan_dlczips(root / "Media" / "DLCZips"); !hit.empty()) return hit;
  const std::filesystem::path pkg_roots[] = {
      root / "user" / "0000000000000000" / "4D5309C9" / "00000002",
      root / "00000002",
      root,
  };
  for (const auto& pr : pkg_roots) {
    if (!std::filesystem::is_directory(pr, ec)) continue;
    for (const auto& pkg : std::filesystem::directory_iterator(pr, ec)) {
      if (!pkg.is_directory(ec)) continue;
      if (auto hit = scan_dlczips(pkg.path() / "Media" / "DLCZips"); !hit.empty()) return hit;
    }
  }
  return {};
}

}  // namespace

const std::vector<VoScriptEntry>* VoScript::find(std::string_view trigger_id) const {
  auto it = triggers.find(lower_str(trigger_id));
  return it != triggers.end() ? &it->second : nullptr;
}

bool parse_dialogue_script(std::string_view xml, VoScript& out, std::string* error) {
  // Strip UTF-8 BOM if present.
  if (xml.size() >= 3 && static_cast<unsigned char>(xml[0]) == 0xEF && static_cast<unsigned char>(xml[1]) == 0xBB &&
      static_cast<unsigned char>(xml[2]) == 0xBF) {
    xml.remove_prefix(3);
  }
  XmlNode root;
  if (!parse_xml(xml, root, error)) return false;
  out.triggers.clear();
  for (const auto& trig : root.children) {
    if (!iequals(trig.name, "Trigger")) continue;
    const std::string id = lower_str(trig.attr("id"));
    if (id.empty()) continue;
    auto& vec = out.triggers[id];
    for (const auto& fn : trig.children) {
      if (!iequals(fn.name, "Filename")) continue;
      const double idx = fn.number("soundbankIndex", -1);
      if (idx < 0) continue;
      VoScriptEntry e;
      e.filename_id = fn.attr("id");
      e.soundbank_index = uint32_t(idx);
      vec.push_back(std::move(e));
    }
    if (vec.empty()) out.triggers.erase(id);
  }
  return !out.triggers.empty();
}

VoPlayer::VoPlayer(int out_rate, bool non_blocking)
    : rate_(std::max(8000, out_rate)), non_blocking_(non_blocking) {}

VoPlayer::~VoPlayer() {
  clear();
  pool_.reset();
}

void VoPlayer::ensure_pool() {
  if (pool_) return;
  pool_ = std::make_unique<StreamPool>(4, 65536, [this](uint64_t key) -> std::unique_ptr<PcmSource> {
    const uint32_t bank_id = uint32_t((key >> 32) & 0xFFFFFFFFu);
    const uint32_t idx = uint32_t(key & 0xFFFFFFFFu);
    const auto& samples = (bank_id == 1) ? rally_fsb_samples_ : fsb_samples_;
    const auto& path = (bank_id == 1) ? rally_fsb_path_ : fsb_path_;
    if (path.empty() || idx >= samples.size()) return nullptr;
    const FsbSample& s = samples[idx];
    if (s.xma()) return std::make_unique<FsbXmaSource>(path, idx);
    return make_fsb_mpeg_source(path, s.offset, s.bytes, s.rate > 0 ? s.rate : 48000);
  });
}

bool VoPlayer::load(const std::filesystem::path& game_root, std::string_view language, std::string* note) {
  clear();
  pool_.reset();
  fsb_samples_.clear();
  rally_fsb_samples_.clear();
  rally_fsb_path_.clear();
  has_rally_vo_ = false;
  script_ = VoScript{};
  if (game_root.empty()) return false;

  const auto vo_dir = game_root / "media" / "audio" / "vo";
  std::string lang = upper_str(language.empty() ? "EN" : language);
  std::error_code ec;
  auto xml_path = cars::mods::resolve(vo_dir / ("DialogueScript_" + lang + ".xml"));
  auto fsb_path = cars::mods::resolve(vo_dir / ("VO_" + lang + "_Stream.fsb"));
  if (!std::filesystem::exists(xml_path, ec) || !std::filesystem::exists(fsb_path, ec)) {
    lang = "EN";
    xml_path = cars::mods::resolve(vo_dir / "DialogueScript_EN.xml");
    fsb_path = cars::mods::resolve(vo_dir / "VO_EN_Stream.fsb");
  }
  if (!std::filesystem::exists(xml_path, ec) || !std::filesystem::exists(fsb_path, ec)) {
    if (note) *note = "vo: DialogueScript_EN.xml or VO_EN_Stream.fsb not found";
    return false;
  }

  std::string err;
  if (!parse_dialogue_script(read_text(xml_path), script_, &err)) {
    if (note) *note = "vo: " + err;
    return false;
  }
  script_.language = lang;

  if (!read_fsb_headers(fsb_path, fsb_samples_)) {
    if (note) *note = "vo: failed to read FSB4 headers from " + fsb_path.filename().string();
    return false;
  }
  fsb_path_ = fsb_path;
  ensure_pool();

  if (note) {
    *note = "vo: loaded " + std::to_string(script_.trigger_count()) + " triggers (" + lang + ", " +
            std::to_string(fsb_samples_.size()) + " samples)";
  }
  return true;
}

bool VoPlayer::load_rally_vo(const std::filesystem::path& rally_root_or_state_root, std::string_view language,
                             std::string* note) {
  const auto vo_dir = find_rally_vo_dir(rally_root_or_state_root);
  if (vo_dir.empty()) {
    if (note) *note = "rally_vo: Rally_VO banks not found";
    return false;
  }
  std::error_code ec;
  std::string lang = upper_str(language.empty() ? "EN" : language);

  // Path 1: Rally_DialogueScript_<LANG>.xml + Rally_VO_<LANG>_Stream.fsb (or Rally_VO_<LANG>.fsb)
  auto xml_path = vo_dir / ("Rally_DialogueScript_" + lang + ".xml");
  auto fsb_path = vo_dir / ("Rally_VO_" + lang + "_Stream.fsb");
  if (!std::filesystem::exists(fsb_path, ec)) fsb_path = vo_dir / ("Rally_VO_" + lang + ".fsb");
  if (!std::filesystem::exists(xml_path, ec) || !std::filesystem::exists(fsb_path, ec)) {
    lang = "EN";
    xml_path = vo_dir / "Rally_DialogueScript_EN.xml";
    fsb_path = vo_dir / "Rally_VO_EN_Stream.fsb";
    if (!std::filesystem::exists(fsb_path, ec)) fsb_path = vo_dir / "Rally_VO_EN.fsb";
  }

  size_t added = 0;
  if (std::filesystem::exists(xml_path, ec) && std::filesystem::exists(fsb_path, ec)) {
    VoScript rally_script;
    if (parse_dialogue_script(read_text(xml_path), rally_script, nullptr) &&
        read_fsb_headers(fsb_path, rally_fsb_samples_)) {
      rally_fsb_path_ = fsb_path;
      for (auto& [k, vec] : rally_script.triggers) {
        for (auto& e : vec) e.bank_id = 1;
        script_.triggers[k] = std::move(vec);
        ++added;
      }
    }
  } else {
    // Path 2: Rally_VO_<LANG>.fev/.fsb or Rally_VO.fev/.fsb or standalone Rally_VO*.fsb
    auto fev_path = vo_dir / ("Rally_VO_" + lang + ".fev");
    fsb_path = vo_dir / ("Rally_VO_" + lang + ".fsb");
    if (!std::filesystem::exists(fsb_path, ec)) fsb_path = vo_dir / ("Rally_VO_" + lang + "_Stream.fsb");
    if (!std::filesystem::exists(fev_path, ec)) fev_path = vo_dir / "Rally_VO_EN.fev";
    if (!std::filesystem::exists(fsb_path, ec)) fsb_path = vo_dir / "Rally_VO_EN.fsb";
    if (!std::filesystem::exists(fev_path, ec)) fev_path = vo_dir / "Rally_VO.fev";
    if (!std::filesystem::exists(fsb_path, ec)) fsb_path = vo_dir / "Rally_VO.fsb";
    if (!std::filesystem::exists(fsb_path, ec)) fsb_path = vo_dir / "Rally_VO_Stream.fsb";

    if (std::filesystem::exists(fsb_path, ec) && read_fsb_headers(fsb_path, rally_fsb_samples_)) {
      rally_fsb_path_ = fsb_path;
      FevProject proj;
      if (std::filesystem::exists(fev_path, ec) && load_fev(fev_path, proj, nullptr)) {
        for (const auto& ev : proj.events) {
          auto add_insts = [&](const std::vector<FevSoundInstance>& insts) {
            for (const auto& si : insts) {
              if (si.sound_def_index >= proj.sound_defs.size()) continue;
              for (const auto& en : proj.sound_defs[si.sound_def_index].entries) {
                if (en.entry_type != 0 || en.subsound_index >= rally_fsb_samples_.size()) continue;
                const std::string key = lower_str(ev.path);
                script_.triggers[key].push_back({rally_fsb_samples_[en.subsound_index].name, en.subsound_index, 1});
                // Also register short cue tail for convenience.
                if (const auto sl = key.rfind('/'); sl != std::string::npos && sl + 1 < key.size()) {
                  script_.triggers[key.substr(sl + 1)].push_back(
                      {rally_fsb_samples_[en.subsound_index].name, en.subsound_index, 1});
                }
                ++added;
              }
            }
          };
          add_insts(ev.simple_instances);
          for (const auto& ly : ev.layers) add_insts(ly.instances);
        }
      }
      // Also index every named FSB4 subsound so direct subsound trigger names resolve.
      for (uint32_t i = 0; i < rally_fsb_samples_.size(); ++i) {
        const std::string nm = lower_str(rally_fsb_samples_[i].name);
        if (nm.empty()) continue;
        if (!script_.triggers.count(nm)) {
          script_.triggers[nm].push_back({rally_fsb_samples_[i].name, i, 1});
          ++added;
        }
      }
    }
  }

  if (added == 0 || rally_fsb_samples_.empty()) {
    if (note) *note = "rally_vo: no playable Rally_VO triggers found in " + vo_dir.string();
    return false;
  }
  has_rally_vo_ = true;
  ensure_pool();
  if (note) {
    *note = "rally_vo: loaded " + std::to_string(added) + " triggers (" + lang + ", " +
            std::to_string(rally_fsb_samples_.size()) + " samples)";
  }
  return true;
}

void VoPlayer::set_clip(std::string_view trigger_id, PcmSample sample) {
  mem_clips_[lower_str(trigger_id)] = std::move(sample);
}

bool VoPlayer::has_trigger(std::string_view trigger_id) const {
  const std::string key = lower_str(trigger_id);
  return mem_clips_.count(key) > 0 || script_.triggers.count(key) > 0;
}

bool VoPlayer::trigger(std::string_view trigger_id, bool interrupt, int variant) {
  const std::string key = lower_str(trigger_id);
  QueuedLine ql;
  ql.trigger_id = key;

  if (mem_clips_.count(key) > 0) {
    ql.sample_index = -1;
  } else if (const auto* entries = script_.find(key); entries && !entries->empty() && pool_) {
    const auto& pick = variant >= 0 && size_t(variant) < entries->size()
                            ? (*entries)[size_t(variant)]
                            : (*entries)[size_t(variant_rr_++ % entries->size())];
    const auto& bank_samples = (pick.bank_id == 1) ? rally_fsb_samples_ : fsb_samples_;
    if (pick.soundbank_index >= bank_samples.size()) return false;
    ql.sample_index = int64_t(pick.soundbank_index);
    ql.bank_id = pick.bank_id;
  } else {
    return false;
  }

  if (interrupt) {
    if (active_.load(std::memory_order_relaxed)) begin_fade_out();
    stop_active();
    queue_.clear();
  }
  if (queue_.size() >= 8) queue_.pop_front();
  queue_.push_back(std::move(ql));
  if (!active_.load(std::memory_order_relaxed)) start_next();
  return true;
}

std::string VoPlayer::active_trigger() const {
  char tmp[64]{};
  for (int tries = 0; tries < 8; ++tries) {
    const uint32_t s0 = trigger_seq_.load(std::memory_order_acquire);
    if (s0 & 1u) continue;
    std::memcpy(tmp, trigger_buf_.data(), sizeof(tmp));
    tmp[sizeof(tmp) - 1] = '\0';
    const uint32_t s1 = trigger_seq_.load(std::memory_order_acquire);
    if (s0 == s1) return std::string(tmp);
  }
  return {};
}

void VoPlayer::publish_trigger_snapshot(std::string_view trig) {
  const uint32_t seq = trigger_seq_.load(std::memory_order_relaxed);
  trigger_seq_.store(seq + 1u, std::memory_order_release);
  const size_t n = std::min(trig.size(), trigger_buf_.size() - 1);
  if (n > 0) std::memcpy(trigger_buf_.data(), trig.data(), n);
  trigger_buf_[n] = '\0';
  trigger_seq_.store(seq + 2u, std::memory_order_release);
  active_trigger_hash_.store(fnv1a_64(trig), std::memory_order_release);
}

void VoPlayer::update_rms(double sum_sq, int frames) {
  const float block_rms = (frames > 0 && sum_sq > 0.0) ? float(std::sqrt(sum_sq / double(frames))) : 0.0f;
  const double dt = double(std::max(1, frames)) / double(rate_);
  const double tau = (block_rms > rms_smooth_) ? 0.015 : 0.060;
  const float alpha = float(1.0 - std::exp(-dt / tau));
  rms_smooth_ += (block_rms - rms_smooth_) * alpha;
  if (!active_.load(std::memory_order_relaxed) && fade_rem_ <= 0 && rms_smooth_ < 1e-4f) {
    rms_smooth_ = 0.0f;
  }
  current_rms_.store(rms_smooth_, std::memory_order_release);
}

void VoPlayer::begin_fade_out() {
  if (!active_.load(std::memory_order_relaxed) || pos_ <= 0.0) return;
  clear_fade_out();
  fade_total_ = std::max(1, rate_ / 100);  // 10 ms smooth linear fade-out
  fade_rem_ = fade_total_;
  fade_pos_ = pos_;
  fade_last_l_ = last_l_;
  fade_last_r_ = last_r_;
  if (active_mem_) {
    fade_mem_ = active_mem_;
    active_mem_ = nullptr;
  } else if (active_slot_ >= 0) {
    fade_slot_ = active_slot_;
    active_slot_ = -1;  // ownership transferred to fade tail until fade completes
  }
}

void VoPlayer::clear_fade_out() {
  if (fade_slot_ >= 0 && pool_) pool_->release(fade_slot_);
  fade_slot_ = -1;
  fade_mem_ = nullptr;
  fade_rem_ = 0;
  fade_total_ = 0;
  fade_pos_ = 0.0;
  fade_last_l_ = 0.0f;
  fade_last_r_ = 0.0f;
}

bool VoPlayer::render_fade_out(float* out, int frames, double& sum_sq) {
  if (fade_rem_ <= 0 || fade_total_ <= 0) return false;
  const int n = std::min(frames, fade_rem_);
  const double mem_step = fade_mem_ ? (double(std::max(8000, fade_mem_->rate)) / double(rate_)) : 1.0;
  PcmStream* st = (fade_slot_ >= 0 && pool_) ? pool_->stream(fade_slot_) : nullptr;
  const bool st_ready = st && st->state() == PcmStream::kReady && st->positioned();
  const double st_step = st_ready ? (double(std::max(8000, st->rate())) / double(rate_)) : 1.0;
  const int64_t rfrom = st_ready ? st->from() : 0;
  const int64_t rend = st_ready ? st->end() : 0;

  for (int i = 0; i < n; ++i) {
    float vl = fade_last_l_;
    float vr = fade_last_r_;
    if (fade_mem_) {
      const auto& data = fade_mem_->data;
      const size_t idx = size_t(fade_pos_);
      if (idx < data.size()) {
        const float s0 = data[idx];
        const float s1 = idx + 1 < data.size() ? data[idx + 1] : s0;
        const float frac = float(fade_pos_ - double(idx));
        const float s = (s0 + (s1 - s0) * frac) * kMonoPanGain;
        vl = vr = s;
        fade_last_l_ = fade_last_r_ = s;
        fade_pos_ += mem_step;
      }
    } else if (st_ready) {
      const int64_t a = int64_t(std::floor(fade_pos_));
      if (a >= rfrom && a < rend) {
        const float frac = float(fade_pos_ - double(a));
        float al = 0.0f, ar = 0.0f, bl = 0.0f, br = 0.0f;
        st->sample(a, al, ar);
        if (a + 1 < rend) {
          st->sample(a + 1, bl, br);
          al += (bl - al) * frac;
          ar += (br - ar) * frac;
        }
        vl = al;
        vr = ar;
        fade_last_l_ = al;
        fade_last_r_ = ar;
        fade_pos_ += st_step;
      }
    }
    const float g = float(fade_rem_ - 1) / float(fade_total_);
    const float fl = vl * g;
    const float fr = vr * g;
    out[2 * i] += fl;
    out[2 * i + 1] += fr;
    sum_sq += 0.5 * (double(fl) * double(fl) + double(fr) * double(fr));
    --fade_rem_;
  }
  if (fade_rem_ <= 0) clear_fade_out();
  return n > 0;
}

void VoPlayer::skip_current() {
  if (!active_.load(std::memory_order_relaxed)) return;
  begin_fade_out();
  start_next();
}

void VoPlayer::clear() {
  clear_fade_out();
  stop_active();
  queue_.clear();
  rms_smooth_ = 0.0f;
  current_rms_.store(0.0f, std::memory_order_release);
}

void VoPlayer::stop_active() {
  if (active_slot_ >= 0 && pool_) pool_->release(active_slot_);
  active_slot_ = -1;
  active_mem_ = nullptr;
  active_.store(false, std::memory_order_release);
  active_trigger_.clear();
  publish_trigger_snapshot({});
  elapsed_s_.store(0.0f, std::memory_order_release);
  duration_s_.store(0.0f, std::memory_order_release);
  pos_ = 0.0;
  stalled_frames_ = 0;
  seek_retries_ = 0;
  last_l_ = 0.0f;
  last_r_ = 0.0f;
  if (fade_rem_ <= 0) {
    rms_smooth_ = 0.0f;
    current_rms_.store(0.0f, std::memory_order_release);
  }
}

void VoPlayer::start_next() {
  stop_active();
  while (!queue_.empty()) {
    QueuedLine ql = std::move(queue_.front());
    queue_.pop_front();
    if (ql.sample_index < 0) {
      auto it = mem_clips_.find(ql.trigger_id);
      if (it == mem_clips_.end() || it->second.data.empty()) continue;
      active_mem_ = &it->second;
      active_trigger_ = std::move(ql.trigger_id);
      pos_ = 0.0;
      const int clip_rate = std::max(8000, active_mem_->rate);
      duration_s_.store(float(double(active_mem_->data.size()) / double(clip_rate)), std::memory_order_release);
      elapsed_s_.store(0.0f, std::memory_order_release);
      publish_trigger_snapshot(active_trigger_);
      active_.store(true, std::memory_order_release);
      return;
    }
    if (pool_) {
      const uint64_t key = (uint64_t(ql.bank_id) << 32) | uint64_t(ql.sample_index);
      const int slot = pool_->acquire(key, 0);
      if (slot < 0) continue;
      active_slot_ = slot;
      active_trigger_ = std::move(ql.trigger_id);
      pos_ = 0.0;
      const auto& bank_samples = (ql.bank_id == 1) ? rally_fsb_samples_ : fsb_samples_;
      float est_dur = 0.0f;
      if (ql.sample_index >= 0 && size_t(ql.sample_index) < bank_samples.size()) {
        const FsbSample& fs = bank_samples[size_t(ql.sample_index)];
        const int sr = fs.rate > 0 ? fs.rate : 48000;
        if (fs.frames > 0) est_dur = float(double(fs.frames) / double(sr));
      }
      duration_s_.store(est_dur, std::memory_order_release);
      elapsed_s_.store(0.0f, std::memory_order_release);
      publish_trigger_snapshot(active_trigger_);
      active_.store(true, std::memory_order_release);
      return;
    }
  }
}

bool VoPlayer::render(float* out, int frames) {
  if (!out || frames <= 0) return active_.load(std::memory_order_acquire) || fade_rem_ > 0;
  double sum_sq = 0.0;
  const bool rendered_fade = render_fade_out(out, frames, sum_sq);

  if (!active_.load(std::memory_order_relaxed)) {
    if (!queue_.empty()) start_next();
    if (!active_.load(std::memory_order_relaxed)) {
      update_rms(sum_sq, frames);
      return rendered_fade;
    }
  }

  if (active_mem_) {
    const auto& data = active_mem_->data;
    const int clip_rate = std::max(8000, active_mem_->rate);
    const double step = double(clip_rate) / double(rate_);
    int rendered = 0;
    for (int i = 0; i < frames; ++i) {
      const size_t idx = size_t(pos_);
      if (idx >= data.size()) {
        start_next();
        break;
      }
      const float s0 = data[idx];
      const float s1 = idx + 1 < data.size() ? data[idx + 1] : s0;
      const float frac = float(pos_ - double(idx));
      const float sample = (s0 + (s1 - s0) * frac) * kMonoPanGain;
      out[2 * i] += sample;
      out[2 * i + 1] += sample;
      last_l_ = last_r_ = sample;
      sum_sq += double(sample) * double(sample);
      pos_ += step;
      ++rendered;
    }
    if (active_mem_) {
      elapsed_s_.store(float(pos_ / double(clip_rate)), std::memory_order_release);
    }
    update_rms(sum_sq, frames);
    return active_.load(std::memory_order_relaxed) || rendered_fade || rendered > 0;
  }

  if (active_slot_ >= 0 && pool_) {
    PcmStream* st = pool_->stream(active_slot_);
    if (!st) {
      start_next();
      update_rms(sum_sq, frames);
      return active_.load(std::memory_order_relaxed) || rendered_fade;
    }
    if (!non_blocking_ && (st->state() == PcmStream::kLoading || !st->positioned())) {
      pool_->wait_ready(active_slot_);
    }
    if (st->state() == PcmStream::kFailed) {
      start_next();
      update_rms(sum_sq, frames);
      return active_.load(std::memory_order_relaxed) || rendered_fade;
    }
    if (st->state() != PcmStream::kReady || !st->positioned()) {
      stalled_frames_ += frames;
      if (stalled_frames_ > rate_ * 4) {
        // Bounded stall guard so a failing/starved stream cannot hang VOPlaying forever.
        start_next();
        update_rms(sum_sq, frames);
        return active_.load(std::memory_order_relaxed) || rendered_fade;
      }
      update_rms(sum_sq, frames);
      return true;  // still loading/seeking on background worker
    }
    const int st_rate = std::max(8000, st->rate());
    if (st->length() > 0) {
      duration_s_.store(float(double(st->length()) / double(st_rate)), std::memory_order_release);
    }
    const double step = double(st_rate) / double(rate_);
    const int64_t rfrom = st->from();
    const int64_t rend = st->end();
    bool ring_behind = false;
    bool ring_starved = false;
    int rendered = 0;
    for (int i = 0; i < frames; ++i) {
      const int64_t a = int64_t(std::floor(pos_));
      if (a >= rend && st->source_done()) {
        start_next();
        break;
      }
      if (a < rfrom) {
        // Ring window wrapped/advanced past pos_: recover by re-seeking or snapping
        // cleanly instead of stalling forever in silence!
        ring_behind = true;
        break;
      }
      if (a >= rend) {
        // Worker has not filled up to `a` yet.
        ring_starved = true;
        break;
      }
      const float frac = float(pos_ - double(a));
      float al = 0.0f, ar = 0.0f, bl = 0.0f, br = 0.0f;
      st->sample(a, al, ar);
      if (a + 1 < rend) {
        st->sample(a + 1, bl, br);
        al += (bl - al) * frac;
        ar += (br - ar) * frac;
      }
      out[2 * i] += al;
      out[2 * i + 1] += ar;
      last_l_ = al;
      last_r_ = ar;
      sum_sq += 0.5 * (double(al) * double(al) + double(ar) * double(ar));
      pos_ += step;
      stalled_frames_ = 0;
      seek_retries_ = 0;
      ++rendered;
    }
    if (active_.load(std::memory_order_relaxed) && st) {
      elapsed_s_.store(float(pos_ / double(st_rate)), std::memory_order_release);
      if (ring_behind) {
        const int64_t cur = int64_t(std::floor(pos_));
        if (seek_retries_ < 2) {
          ++seek_retries_;
          st->request(std::max<int64_t>(0, cur));
        } else {
          // After retry, snap forward to valid ring window start so playback always makes progress.
          pos_ = double(rfrom);
          seek_retries_ = 0;
        }
      } else if (ring_starved) {
        stalled_frames_ += frames;
        st->hint(std::max<int64_t>(rfrom, int64_t(std::floor(pos_)) - 4));
        if (stalled_frames_ > rate_ * 2) {
          start_next();
        }
      } else {
        st->hint(std::max<int64_t>(rfrom, int64_t(std::floor(pos_)) - 4));
      }
    }
    update_rms(sum_sq, frames);
    return active_.load(std::memory_order_relaxed) || rendered_fade || rendered > 0;
  }

  update_rms(sum_sq, frames);
  return rendered_fade;
}

}  // namespace audio
