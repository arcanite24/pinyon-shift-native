#include "audio/user_music.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <numeric>
#include <system_error>
#include <thread>

#include "audio/stream.h"

namespace audio {
namespace {

std::string lower_str(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

UserMusicPlayer::UserMusicPlayer(const std::filesystem::path& folder, int out_rate, uint64_t seed, bool non_blocking)
    : rate_(std::max(1, out_rate)), non_blocking_(non_blocking), rng_(seed) {
  scan(folder);
}

UserMusicPlayer::~UserMusicPlayer() {
  stop_streams();
  pool_.reset();
}

int UserMusicPlayer::scan(const std::filesystem::path& folder) {
  stop_streams();
  pool_.reset();
  folder_ = folder;
  tracks_.clear();
  std::error_code ec;
  if (!folder.empty() && std::filesystem::is_directory(folder, ec)) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             folder, std::filesystem::directory_options::skip_permission_denied, ec)) {
      if (entry.is_regular_file(ec) && is_music_file(entry.path())) tracks_.push_back(entry.path());
    }
  }
  std::sort(tracks_.begin(), tracks_.end(), [&](const std::filesystem::path& a, const std::filesystem::path& b) {
    return lower_str(a.lexically_relative(folder_).generic_string()) <
           lower_str(b.lexically_relative(folder_).generic_string());
  });
  titles_.clear();
  titles_.reserve(tracks_.size());
  for (const auto& p : tracks_) titles_.push_back(p.stem().string());
  order_pos_ = 0;
  rebuild_order(-1);
  if (!tracks_.empty()) {
    pool_ = std::make_unique<StreamPool>(4, 98304, [this](uint64_t key) -> std::unique_ptr<PcmSource> {
      if (key >= tracks_.size()) return nullptr;
      return make_file_source(tracks_[size_t(key)]);
    });
  }
  if (active()) start_current();
  return int(tracks_.size());
}

void UserMusicPlayer::rebuild_order(int keep_track) {
  order_.resize(tracks_.size());
  std::iota(order_.begin(), order_.end(), 0);
  if (shuffle_ && order_.size() > 1) std::shuffle(order_.begin(), order_.end(), rng_);
  order_pos_ = 0;
  if (keep_track >= 0) {
    for (size_t i = 0; i < order_.size(); ++i) {
      if (order_[i] == keep_track) {
        order_pos_ = int(i);
        break;
      }
    }
  }
}

int UserMusicPlayer::next_order_pos(int pos) const {
  if (order_.empty()) return 0;
  return (pos + 1) % int(order_.size());
}

void UserMusicPlayer::stop_streams() {
  if (pool_) {
    if (cur_slot_ >= 0) pool_->release(cur_slot_);
    if (next_slot_ >= 0) pool_->release(next_slot_);
  }
  cur_slot_ = -1;
  next_slot_ = -1;
  next_track_ = -1;
  pos_ = 0.0;
  fade_ = 0.0f;
}

void UserMusicPlayer::start_current() {
  if (!active() || !pool_) return;
  const int track = current_index();
  if (track < 0) return;
  if (cur_slot_ >= 0) {
    pool_->release(cur_slot_);
    cur_slot_ = -1;
  }
  if (next_slot_ >= 0 && next_track_ == track) {
    cur_slot_ = next_slot_;
    next_slot_ = -1;
    next_track_ = -1;
  } else {
    if (next_slot_ >= 0) {
      pool_->release(next_slot_);
      next_slot_ = -1;
      next_track_ = -1;
    }
    cur_slot_ = pool_->acquire(uint64_t(track), 0);
  }
  pos_ = 0.0;
  fade_ = 0.0f;
  ensure_prefetch();
}

void UserMusicPlayer::ensure_prefetch() {
  if (!active() || !pool_ || order_.size() <= 1) return;
  const int want_track = order_[size_t(next_order_pos(order_pos_))];
  if (next_slot_ >= 0 && next_track_ == want_track) return;
  if (next_slot_ >= 0) {
    pool_->release(next_slot_);
    next_slot_ = -1;
    next_track_ = -1;
  }
  next_slot_ = pool_->acquire(uint64_t(want_track), 0);
  next_track_ = next_slot_ >= 0 ? want_track : -1;
}

void UserMusicPlayer::set_enabled(bool on) {
  if (enabled_ == on) return;
  enabled_ = on;
  if (active()) start_current();
  else stop_streams();
}

void UserMusicPlayer::set_shuffle(bool on) {
  if (shuffle_ == on) return;
  const int cur = current_index();
  shuffle_ = on;
  rebuild_order(cur);
  if (active()) ensure_prefetch();
}

void UserMusicPlayer::next() {
  if (order_.empty()) return;
  order_pos_ = (order_pos_ + 1) % int(order_.size());
  if (active()) start_current();
}

void UserMusicPlayer::prev() {
  if (order_.empty()) return;
  order_pos_ = (order_pos_ - 1 + int(order_.size())) % int(order_.size());
  if (active()) start_current();
}

int UserMusicPlayer::current_index() const {
  if (order_.empty()) return -1;
  return order_[size_t(std::clamp(order_pos_, 0, int(order_.size()) - 1))];
}

std::string_view UserMusicPlayer::current_title_view() const {
  const int idx = current_index();
  if (idx < 0 || size_t(idx) >= titles_.size()) return {};
  return titles_[size_t(idx)];
}

std::string UserMusicPlayer::current_title() const { return std::string(current_title_view()); }

void UserMusicPlayer::render(float* out, int frames) {
  if (!active() || !pool_ || frames <= 0) return;
  if (cur_slot_ < 0) start_current();
  if (cur_slot_ < 0) return;
  const float fade_step = 1.0f / std::max(1.0f, 0.015f * float(rate_));
  int done_frames = 0;
  int skips = 0;
  while (done_frames < frames) {
    PcmStream* st = pool_->stream(cur_slot_);
    if (!st) break;
    if (!non_blocking_ && (st->state() == PcmStream::kLoading || !st->positioned())) pool_->wait_ready(cur_slot_);
    const auto state = st->state();
    if (state == PcmStream::kFailed) {
      if (++skips >= int(order_.size())) break;  // all files unreadable
      next();
      continue;
    }
    if (state != PcmStream::kReady || !st->positioned()) break;
    const double step = double(st->rate()) / rate_;
    const int rem = frames - done_frames;
    if (!non_blocking_) {
      const int64_t want_end = int64_t(pos_ + step * rem) + 2;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (!st->source_done() && st->end() < want_end && std::chrono::steady_clock::now() < deadline) {
        st->hint(std::max<int64_t>(st->from(), int64_t(std::floor(pos_)) - 4));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    const int64_t rfrom = st->from(), rend = st->end();
    const bool eof = st->source_done();
    bool hit_eof = false;
    for (int i = 0; i < rem; ++i, ++done_frames, pos_ += step) {
      const int64_t a = int64_t(std::floor(pos_));
      if (a >= rend) {
        if (eof) {
          hit_eof = true;
          break;
        }
        continue;
      }
      if (a < rfrom) continue;
      const double frac = pos_ - double(a);
      float al = 0, ar = 0, bl = 0, br = 0;
      st->sample(a, al, ar);
      if (a + 1 < rend) {
        st->sample(a + 1, bl, br);
        al = float(al + (bl - al) * frac);
        ar = float(ar + (br - ar) * frac);
      }
      fade_ = std::min(1.0f, fade_ + fade_step);
      const float g = volume_ * fade_;
      out[2 * done_frames] += g * al;
      out[2 * done_frames + 1] += g * ar;
    }
    st->hint(std::max<int64_t>(rfrom, int64_t(std::floor(pos_)) - 4));
    if (hit_eof) {
      if (order_.size() == 1) {
        // Loop single track cleanly.
        pos_ = 0.0;
        fade_ = 0.0f;
        st->request(0);
      } else {
        next();
      }
      if (++skips > int(order_.size())) break;
    } else {
      break;
    }
  }
}

}  // namespace audio
