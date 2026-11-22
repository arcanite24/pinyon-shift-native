#include "audio/stream.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>

#include "audio/radio.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#endif
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "dr_mp3.h"
#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#include "dr_wav.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace audio {
namespace {

std::string lower_ext(const std::filesystem::path& p) {
  std::string s = p.extension().string();
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

struct StreamFile {
  std::ifstream f;
  int64_t size = 0;

  bool open(const std::filesystem::path& p) {
    f.open(p, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    size = int64_t(f.tellg());
    f.seekg(0, std::ios::beg);
    return size >= 0;
  }
  size_t read(void* out, size_t n) {
    if (n == 0) return 0;
    f.read(static_cast<char*>(out), std::streamsize(n));
    return size_t(std::max<std::streamsize>(0, f.gcount()));
  }
  bool seek(int offset, int whence) {
    f.clear();
    const auto dir = whence == 0 ? std::ios::beg : whence == 1 ? std::ios::cur : std::ios::end;
    f.seekg(offset, dir);
    return !f.fail();
  }
  bool tell(int64_t* cursor) {
    f.clear();
    const auto pos = f.tellg();
    if (pos < 0) return false;
    *cursor = int64_t(pos);
    return true;
  }
};

size_t mp3_read(void* u, void* out, size_t n) { return static_cast<StreamFile*>(u)->read(out, n); }
drmp3_bool32 mp3_seek(void* u, int off, drmp3_seek_origin orig) {
  const int w = orig == DRMP3_SEEK_SET ? 0 : orig == DRMP3_SEEK_CUR ? 1 : 2;
  return static_cast<StreamFile*>(u)->seek(off, w) ? DRMP3_TRUE : DRMP3_FALSE;
}
drmp3_bool32 mp3_tell(void* u, drmp3_int64* cursor) {
  int64_t pos = 0;
  if (!static_cast<StreamFile*>(u)->tell(&pos)) return DRMP3_FALSE;
  *cursor = pos;
  return DRMP3_TRUE;
}

size_t wav_read(void* u, void* out, size_t n) { return static_cast<StreamFile*>(u)->read(out, n); }
drwav_bool32 wav_seek(void* u, int off, drwav_seek_origin orig) {
  const int w = orig == DRWAV_SEEK_SET ? 0 : orig == DRWAV_SEEK_CUR ? 1 : 2;
  return static_cast<StreamFile*>(u)->seek(off, w) ? DRWAV_TRUE : DRWAV_FALSE;
}
drwav_bool32 wav_tell(void* u, drwav_int64* cursor) {
  int64_t pos = 0;
  if (!static_cast<StreamFile*>(u)->tell(&pos)) return DRWAV_FALSE;
  *cursor = pos;
  return DRWAV_TRUE;
}

// Converts `got` interleaved `ch`-channel frames in `tmp` to stereo in `dst`.
void to_stereo(const float* src, int ch, int64_t got, float* dst) {
  if (ch == 2) {
    std::memcpy(dst, src, size_t(got) * 2 * sizeof(float));
    return;
  }
  for (int64_t i = 0; i < got; ++i) {
    dst[2 * i] = src[i * ch];
    dst[2 * i + 1] = ch > 1 ? src[i * ch + 1] : src[i * ch];
  }
}

class FsbMpegSource final : public PcmSource {
 public:
  FsbMpegSource(std::filesystem::path bank, uint64_t offset, uint32_t bytes, int fallback_rate)
      : bank_(std::move(bank)), offset_(offset), bytes_(bytes), rate_(fallback_rate) {}
  ~FsbMpegSource() override { close_dec(); }

  bool open() override {
    std::vector<uint8_t> raw(bytes_);
    std::ifstream f(bank_, std::ios::binary);
    if (!f) return false;
    f.seekg(std::streamoff(offset_));
    if (!f.read(reinterpret_cast<char*>(raw.data()), std::streamsize(raw.size()))) return false;
    mp3_ = mpeg_frames(raw.data(), raw.size(), &frame_at_, &rate_, &spf_);
    if (frame_at_.empty() || spf_ <= 0 || rate_ <= 0) return false;
    return seek(0);
  }
  int rate() const override { return rate_; }
  int64_t length() const override { return int64_t(frame_at_.size()) * spf_; }

  bool seek(int64_t frame) override {
    close_dec();
    if (frame_at_.empty()) return false;
    frame = std::max<int64_t>(0, frame);
    // Two MPEG frames early so the bit reservoir primes cleanly; discard the preroll.
    const int64_t k = std::clamp<int64_t>(frame / spf_ - 2, 0, int64_t(frame_at_.size()) - 1);
    const size_t byte_off = frame_at_[size_t(k)];
    open_ = drmp3_init_memory(&dec_, mp3_.data() + byte_off, mp3_.size() - byte_off, nullptr) != 0;
    if (!open_) return false;
    int64_t skip = frame - k * spf_;
    float discard[2048 * 2];
    while (skip > 0) {
      const drmp3_uint64 ask = drmp3_uint64(std::min<int64_t>(skip, 2048));
      const drmp3_uint64 got = drmp3_read_pcm_frames_f32(&dec_, ask, discard);
      if (got == 0) break;
      skip -= int64_t(got);
    }
    return true;
  }

  int64_t read(float* stereo, int64_t frames) override {
    if (!open_ || frames <= 0) return 0;
    const int ch = std::max(1, int(dec_.channels));
    scratch_.resize(size_t(frames) * size_t(ch));
    const drmp3_uint64 got = drmp3_read_pcm_frames_f32(&dec_, drmp3_uint64(frames), scratch_.data());
    if (got == 0) return 0;
    to_stereo(scratch_.data(), ch, int64_t(got), stereo);
    return int64_t(got);
  }

 private:
  void close_dec() {
    if (open_) {
      drmp3_uninit(&dec_);
      open_ = false;
    }
  }
  std::filesystem::path bank_;
  uint64_t offset_ = 0;
  uint32_t bytes_ = 0;
  std::vector<uint8_t> mp3_;
  std::vector<uint32_t> frame_at_;
  int rate_ = 48000, spf_ = 1152;
  drmp3 dec_{};
  bool open_ = false;
  std::vector<float> scratch_;
};

class Mp3FileSource final : public PcmSource {
 public:
  explicit Mp3FileSource(std::filesystem::path path) : path_(std::move(path)) {}
  ~Mp3FileSource() override {
    if (open_) drmp3_uninit(&dec_);
  }

  bool open() override {
    if (!file_.open(path_)) return false;
    open_ = drmp3_init(&dec_, mp3_read, mp3_seek, mp3_tell, nullptr, &file_, nullptr) != 0;
    if (!open_ || dec_.sampleRate == 0 || dec_.channels == 0) {
      if (open_) drmp3_uninit(&dec_);
      open_ = false;
      return false;
    }
    return true;
  }
  int rate() const override { return int(dec_.sampleRate); }
  int64_t length() const override { return -1; }
  bool seek(int64_t frame) override {
    if (!open_) return false;
    return drmp3_seek_to_pcm_frame(&dec_, drmp3_uint64(std::max<int64_t>(0, frame))) != 0;
  }
  int64_t read(float* stereo, int64_t frames) override {
    if (!open_ || frames <= 0) return 0;
    const int ch = std::max(1, int(dec_.channels));
    scratch_.resize(size_t(frames) * size_t(ch));
    const drmp3_uint64 got = drmp3_read_pcm_frames_f32(&dec_, drmp3_uint64(frames), scratch_.data());
    if (got == 0) return 0;
    to_stereo(scratch_.data(), ch, int64_t(got), stereo);
    return int64_t(got);
  }

 private:
  std::filesystem::path path_;
  StreamFile file_;
  drmp3 dec_{};
  bool open_ = false;
  std::vector<float> scratch_;
};

class WavFileSource final : public PcmSource {
 public:
  explicit WavFileSource(std::filesystem::path path) : path_(std::move(path)) {}
  ~WavFileSource() override {
    if (open_) drwav_uninit(&wav_);
  }

  bool open() override {
    if (!file_.open(path_)) return false;
    open_ = drwav_init(&wav_, wav_read, wav_seek, wav_tell, &file_, nullptr) != 0;
    if (!open_ || wav_.sampleRate == 0 || wav_.channels == 0) {
      if (open_) drwav_uninit(&wav_);
      open_ = false;
      return false;
    }
    return true;
  }
  int rate() const override { return int(wav_.sampleRate); }
  int64_t length() const override { return int64_t(wav_.totalPCMFrameCount); }
  bool seek(int64_t frame) override {
    if (!open_) return false;
    return drwav_seek_to_pcm_frame(&wav_, drwav_uint64(std::max<int64_t>(0, frame))) != 0;
  }
  int64_t read(float* stereo, int64_t frames) override {
    if (!open_ || frames <= 0) return 0;
    const int ch = std::max(1, int(wav_.channels));
    scratch_.resize(size_t(frames) * size_t(ch));
    const drwav_uint64 got = drwav_read_pcm_frames_f32(&wav_, drwav_uint64(frames), scratch_.data());
    if (got == 0) return 0;
    to_stereo(scratch_.data(), ch, int64_t(got), stereo);
    return int64_t(got);
  }

 private:
  std::filesystem::path path_;
  StreamFile file_;
  drwav wav_{};
  bool open_ = false;
  std::vector<float> scratch_;
};

}  // namespace

std::unique_ptr<PcmSource> make_fsb_mpeg_source(const std::filesystem::path& bank, uint64_t offset, uint32_t bytes,
                                                int fallback_rate) {
  return std::make_unique<FsbMpegSource>(bank, offset, bytes, fallback_rate);
}

bool is_music_file(const std::filesystem::path& file) {
  const std::string e = lower_ext(file);
  return e == ".mp3" || e == ".wav";
}

std::unique_ptr<PcmSource> make_file_source(const std::filesystem::path& file) {
  const std::string e = lower_ext(file);
  if (e == ".mp3") return std::make_unique<Mp3FileSource>(file);
  if (e == ".wav") return std::make_unique<WavFileSource>(file);
  return nullptr;
}

// ------------------------------------------------------------------ stream

void PcmStream::load(std::unique_ptr<PcmSource> src, size_t ring_frames) {
  src_ = std::move(src);
  cap_ = std::max<size_t>(4096, ring_frames);
  if (!src_ || !src_->open()) {
    src_.reset();
    state_.store(kFailed, std::memory_order_release);
    return;
  }
  rate_ = std::max(1, src_->rate());
  length_ = src_->length();
  ring_.assign(cap_ * 2, 0.0f);
  const uint32_t g = seek_gen_.load(std::memory_order_acquire);
  const int64_t target = g != 0 ? seek_to_.load(std::memory_order_relaxed) : 0;
  seek_and_prefill(target);
  served_gen_ = g;
  epoch_.store(g, std::memory_order_release);
  state_.store(kReady, std::memory_order_release);
}

void PcmStream::reset() {
  src_.reset();
  ring_.clear();
  cap_ = 0;
  from_.store(0, std::memory_order_relaxed);
  end_.store(0, std::memory_order_relaxed);
  want_.store(0, std::memory_order_relaxed);
  done_.store(false, std::memory_order_relaxed);
  served_gen_ = 0;
  state_.store(kEmpty, std::memory_order_release);
}

void PcmStream::request(int64_t first) {
  seek_to_.store(std::max<int64_t>(0, first), std::memory_order_relaxed);
  ++requested_gen_;
  seek_gen_.store(requested_gen_, std::memory_order_release);
}

void PcmStream::seek_and_prefill(int64_t target) {
  target = std::max<int64_t>(0, target);
  const bool ok = src_ && src_->seek(target);
  from_.store(target, std::memory_order_relaxed);
  end_.store(target, std::memory_order_relaxed);
  want_.store(target, std::memory_order_relaxed);
  done_.store(!ok, std::memory_order_relaxed);
  if (ok) fill_chunk(std::min<int64_t>(int64_t(cap_) / 2, 16384));
}

bool PcmStream::fill_chunk(int64_t max_frames) {
  if (!src_ || done_.load(std::memory_order_relaxed) || cap_ == 0) return false;
  const int64_t w = end_.load(std::memory_order_relaxed);
  const int64_t floor = std::min(w, std::max(from_.load(std::memory_order_relaxed), want_.load(std::memory_order_acquire)));
  const int64_t free_frames = int64_t(cap_) - (w - floor);
  if (free_frames <= 0) return false;
  const int64_t ask = std::min(free_frames, max_frames);
  float tmp[4096 * 2];
  int64_t total = 0;
  while (total < ask) {
    const int64_t n = std::min<int64_t>(ask - total, 4096);
    const int64_t got = src_->read(tmp, n);
    if (got <= 0) {
      done_.store(true, std::memory_order_release);
      break;
    }
    const size_t at = size_t((w + total) % int64_t(cap_));
    const size_t first_part = std::min(size_t(got), cap_ - at);
    std::memcpy(ring_.data() + at * 2, tmp, first_part * 2 * sizeof(float));
    if (size_t(got) > first_part)
      std::memcpy(ring_.data(), tmp + first_part * 2, (size_t(got) - first_part) * 2 * sizeof(float));
    total += got;
  }
  if (total > 0) {
    const int64_t new_end = w + total;
    const int64_t min_from = new_end - int64_t(cap_);
    if (min_from > from_.load(std::memory_order_relaxed)) from_.store(min_from, std::memory_order_release);
    end_.store(new_end, std::memory_order_release);
    return true;
  }
  return false;
}

bool PcmStream::service() {
  if (state_.load(std::memory_order_relaxed) != kReady) return false;
  const uint32_t g = seek_gen_.load(std::memory_order_acquire);
  if (g != served_gen_) {
    seek_and_prefill(seek_to_.load(std::memory_order_relaxed));
    served_gen_ = g;
    epoch_.store(g, std::memory_order_release);
    return true;
  }
  return fill_chunk(4096);
}

void PcmStream::copy(int64_t first, int64_t n, float* dst) const {
  if (n <= 0 || cap_ == 0) return;
  const size_t at = size_t(first % int64_t(cap_));
  const size_t first_part = std::min(size_t(n), cap_ - at);
  std::memcpy(dst, ring_.data() + at * 2, first_part * 2 * sizeof(float));
  if (size_t(n) > first_part)
    std::memcpy(dst + first_part * 2, ring_.data(), (size_t(n) - first_part) * 2 * sizeof(float));
}

void PcmStream::sample(int64_t frame, float& l, float& r) const {
  const size_t at = size_t(frame % int64_t(cap_)) * 2;
  l = ring_[at];
  r = ring_[at + 1];
}

// -------------------------------------------------------------------- pool

StreamPool::StreamPool(int slots, size_t ring_frames, Factory factory)
    : factory_(std::move(factory)), ring_(ring_frames), slots_(size_t(std::max(1, slots))) {
  worker_ = std::thread([this] { run(); });
}

StreamPool::~StreamPool() {
  stop_.store(true, std::memory_order_release);
  if (worker_.joinable()) worker_.join();
}

int StreamPool::acquire(uint64_t key, int64_t initial_frame) {
  for (size_t i = 0; i < slots_.size(); ++i) {
    if (slots_[i].st.load(std::memory_order_acquire) != kFree) continue;
    PcmStream& s = slots_[i].stream;
    s.requested_gen_ = 0;
    s.seek_gen_.store(0, std::memory_order_relaxed);
    s.epoch_.store(0, std::memory_order_relaxed);
    if (initial_frame > 0) {
      s.seek_to_.store(initial_frame, std::memory_order_relaxed);
      s.requested_gen_ = 1;
      s.seek_gen_.store(1, std::memory_order_relaxed);
    }
    s.state_.store(PcmStream::kLoading, std::memory_order_release);
    slots_[i].key = key;
    slots_[i].st.store(kActive, std::memory_order_release);
    if (!requests_.push({int(i), false, key})) {
      s.state_.store(PcmStream::kEmpty, std::memory_order_relaxed);
      slots_[i].st.store(kFree, std::memory_order_release);
      return -1;
    }
    return int(i);
  }
  return -1;
}

void StreamPool::release(int slot) {
  if (slot < 0 || size_t(slot) >= slots_.size()) return;
  slots_[size_t(slot)].st.store(kReleasing, std::memory_order_release);
  while (!requests_.push({slot, true, 0})) {
    // Queue is 256 deep and there are far fewer slots; should never spin.
    std::this_thread::yield();
  }
}

int StreamPool::in_use() const {
  int n = 0;
  for (const auto& s : slots_)
    if (s.st.load(std::memory_order_acquire) == kActive) ++n;
  return n;
}

bool StreamPool::wait_ready(int slot, double timeout_s) {
  PcmStream* s = stream(slot);
  if (!s) return false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::duration<double>(std::max(0.0, timeout_s));
  while (std::chrono::steady_clock::now() < deadline) {
    const auto st = s->state();
    if (st == PcmStream::kFailed || st == PcmStream::kEmpty) return false;
    if (st == PcmStream::kReady && s->positioned()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return s->state() == PcmStream::kReady && s->positioned();
}

void StreamPool::run() {
  while (!stop_.load(std::memory_order_acquire)) {
    bool busy = false;
    Request req;
    while (requests_.pop(req)) {
      busy = true;
      if (req.slot < 0 || size_t(req.slot) >= slots_.size()) continue;
      Entry& e = slots_[size_t(req.slot)];
      if (req.release) {
        e.stream.reset();
        e.st.store(kFree, std::memory_order_release);
      } else {
        std::unique_ptr<PcmSource> src = factory_ ? factory_(req.key) : nullptr;
        e.stream.load(std::move(src), ring_);
      }
    }
    for (auto& e : slots_) {
      if (e.st.load(std::memory_order_acquire) == kActive)
        if (e.stream.service()) busy = true;
    }
    if (!busy) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

}  // namespace audio
