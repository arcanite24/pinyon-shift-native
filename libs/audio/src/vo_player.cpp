#include "audio/vo_player.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

#include "audio/xml.h"
#include "cars/mods.h"

namespace audio {
namespace {

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

bool VoPlayer::load(const std::filesystem::path& game_root, std::string_view language, std::string* note) {
  clear();
  pool_.reset();
  fsb_samples_.clear();
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
  pool_ = std::make_unique<StreamPool>(4, 65536, [this](uint64_t key) -> std::unique_ptr<PcmSource> {
    const uint32_t idx = uint32_t(key & 0xFFFFFFFFu);
    if (idx >= fsb_samples_.size()) return nullptr;
    const FsbSample& s = fsb_samples_[idx];
    return make_fsb_mpeg_source(fsb_path_, s.offset, s.bytes, s.rate > 0 ? s.rate : 48000);
  });

  if (note) {
    *note = "vo: loaded " + std::to_string(script_.trigger_count()) + " triggers (" + lang + ", " +
            std::to_string(fsb_samples_.size()) + " samples)";
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

bool VoPlayer::trigger(std::string_view trigger_id, bool interrupt) {
  const std::string key = lower_str(trigger_id);
  QueuedLine ql;
  ql.trigger_id = key;

  if (mem_clips_.count(key) > 0) {
    ql.sample_index = -1;
  } else if (const auto* entries = script_.find(key); entries && !entries->empty() && pool_) {
    const auto& pick = (*entries)[size_t(variant_rr_++ % entries->size())];
    if (pick.soundbank_index >= fsb_samples_.size()) return false;
    ql.sample_index = int64_t(pick.soundbank_index);
  } else {
    return false;
  }

  if (interrupt) {
    stop_active();
    queue_.clear();
  }
  if (queue_.size() >= 8) queue_.pop_front();
  queue_.push_back(std::move(ql));
  if (!active_) start_next();
  return true;
}

void VoPlayer::skip_current() {
  if (!active_) return;
  stop_active();
  start_next();
}

void VoPlayer::clear() {
  stop_active();
  queue_.clear();
}

void VoPlayer::stop_active() {
  if (active_slot_ >= 0 && pool_) pool_->release(active_slot_);
  active_slot_ = -1;
  active_mem_ = nullptr;
  active_ = false;
  active_trigger_.clear();
  pos_ = 0.0;
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
      active_ = true;
      return;
    }
    if (pool_) {
      const int slot = pool_->acquire(uint64_t(ql.sample_index), 0);
      if (slot < 0) continue;
      active_slot_ = slot;
      active_trigger_ = std::move(ql.trigger_id);
      pos_ = 0.0;
      active_ = true;
      return;
    }
  }
}

bool VoPlayer::render(float* out, int frames) {
  if (!out || frames <= 0) return active_;
  if (!active_) {
    if (!queue_.empty()) start_next();
    if (!active_) return false;
  }

  if (active_mem_) {
    const auto& data = active_mem_->data;
    const double step = double(std::max(8000, active_mem_->rate)) / double(rate_);
    for (int i = 0; i < frames; ++i) {
      const size_t idx = size_t(pos_);
      if (idx >= data.size()) {
        start_next();
        break;
      }
      const float s0 = data[idx];
      const float s1 = idx + 1 < data.size() ? data[idx + 1] : s0;
      const float frac = float(pos_ - double(idx));
      const float sample = s0 + (s1 - s0) * frac;
      out[2 * i] += sample;
      out[2 * i + 1] += sample;
      pos_ += step;
    }
    return active_;
  }

  if (active_slot_ >= 0 && pool_) {
    PcmStream* st = pool_->stream(active_slot_);
    if (!st) {
      start_next();
      return active_;
    }
    if (!non_blocking_ && (st->state() == PcmStream::kLoading || !st->positioned())) {
      pool_->wait_ready(active_slot_);
    }
    if (st->state() == PcmStream::kFailed) {
      start_next();
      return active_;
    }
    if (st->state() != PcmStream::kReady || !st->positioned()) {
      return true;  // still loading on background worker
    }
    const double step = double(std::max(8000, st->rate())) / double(rate_);
    const int64_t rfrom = st->from();
    const int64_t rend = st->end();
    for (int i = 0; i < frames; ++i) {
      const int64_t a = int64_t(std::floor(pos_));
      if (a >= rend && st->source_done()) {
        start_next();
        break;
      }
      if (a >= rfrom && a < rend) {
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
        pos_ += step;
      }
    }
    if (active_ && st) st->hint(std::max<int64_t>(rfrom, int64_t(std::floor(pos_)) - 4));
    return active_;
  }

  return false;
}

}  // namespace audio
