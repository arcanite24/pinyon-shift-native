#include "audio/xma_decode.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <system_error>
#include <unordered_map>

#include "audio/stream.h"

#ifdef SHIFT_HAVE_INPROCESS_XMA2
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244)
#endif
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#endif

namespace audio {
namespace {

constexpr size_t kXmaPacket = 2048;

void put16_le(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(uint8_t(v & 0xFFu));
  out.push_back(uint8_t((v >> 8) & 0xFFu));
}

void put32_le(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(uint8_t(v & 0xFFu));
  out.push_back(uint8_t((v >> 8) & 0xFFu));
  out.push_back(uint8_t((v >> 16) & 0xFFu));
  out.push_back(uint8_t((v >> 24) & 0xFFu));
}

bool read_file_all(const std::filesystem::path& p, std::vector<uint8_t>& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return !out.empty();
}

// Persistent per-bank FSB4 header index cache: parses each .fsb header once instead
// of re-reading entire 50-200 MiB banks into RAM for every clip decode.
struct CachedFsbBank {
  uint64_t file_size = 0;
  std::filesystem::file_time_type mtime{};
  std::vector<FsbSample> samples;
};

bool get_cached_fsb4_sample(const std::filesystem::path& bank_path, uint32_t subsound, FsbSample& out_sample) {
  std::error_code ec;
  const uint64_t fsz = std::filesystem::file_size(bank_path, ec);
  if (ec || fsz == 0) return false;
  const auto mt = std::filesystem::last_write_time(bank_path, ec);
  const std::string key = bank_path.lexically_normal().string();

  static std::mutex s_bank_mu;
  static std::unordered_map<std::string, CachedFsbBank> s_bank_cache;
  {
    std::lock_guard<std::mutex> lk(s_bank_mu);
    auto it = s_bank_cache.find(key);
    if (it != s_bank_cache.end() && it->second.file_size == fsz && it->second.mtime == mt) {
      if (subsound >= it->second.samples.size()) return false;
      out_sample = it->second.samples[subsound];
      return true;
    }
  }

  std::vector<FsbSample> parsed;
  if (!read_fsb4_file_headers(bank_path, parsed, nullptr)) return false;
  const bool valid = subsound < parsed.size();
  if (valid) out_sample = parsed[subsound];
  {
    std::lock_guard<std::mutex> lk(s_bank_mu);
    s_bank_cache[key] = CachedFsbBank{fsz, mt, std::move(parsed)};
  }
  return valid;
}

bool read_file_range(const std::filesystem::path& p, uint64_t offset, uint32_t bytes, std::vector<uint8_t>& out) {
  if (bytes == 0) return false;
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  f.seekg(std::streamoff(offset), std::ios::beg);
  if (!f) return false;
  out.resize(bytes);
  return bool(f.read(reinterpret_cast<char*>(out.data()), std::streamsize(bytes)));
}

bool write_file_all(const std::filesystem::path& p, const void* data, size_t n) {
  std::ofstream f(p, std::ios::binary);
  f.write(static_cast<const char*>(data), std::streamsize(n));
  return bool(f);
}

bool decode_xma2_via_cli(const FsbSample& s, std::span<const uint8_t> raw_xma,
                         const std::filesystem::path& ffmpeg, const std::filesystem::path& temp_dir,
                         std::vector<float>& interleaved_out, int& out_channels, int& out_rate) {
  if (ffmpeg.empty()) return false;
  std::error_code ec;
  const auto work = temp_dir.empty() ? std::filesystem::temp_directory_path(ec) : temp_dir;
  std::filesystem::create_directories(work, ec);
  const auto id_str = std::to_string(reinterpret_cast<uintptr_t>(raw_xma.data())) + "_" + std::to_string(s.offset);
  const auto in_p = work / ("xma_" + id_str + ".wav");
  const auto out_p = work / ("xma_" + id_str + ".s16");
  std::filesystem::remove(out_p, ec);
  const auto wave = xma2_wave(s, raw_xma);
  if (!write_file_all(in_p, wave.data(), wave.size())) return false;
  std::string cmd = "\"" + ffmpeg.string() + "\" -hide_banner -nostdin -v error -y -i \"" + in_p.string() +
                    "\" -f s16le -acodec pcm_s16le \"" + out_p.string() + "\"";
#ifdef _WIN32
  cmd = "\"" + cmd + "\"";
#endif
  const int rc = std::system(cmd.c_str());
  std::vector<uint8_t> raw;
  const bool read_ok = (rc == 0) && read_file_all(out_p, raw) && raw.size() >= 2;
  std::filesystem::remove(in_p, ec);
  std::filesystem::remove(out_p, ec);
  if (!read_ok) return false;
  const int ch = std::max<int>(1, s.channels);
  const size_t frames = raw.size() / (2 * size_t(ch));
  if (frames == 0) return false;
  out_channels = ch;
  out_rate = s.rate > 0 ? s.rate : 48000;
  interleaved_out.resize(frames * size_t(ch));
  for (size_t i = 0; i < frames * size_t(ch); ++i) {
    const int16_t v = int16_t(raw[2 * i] | (raw[2 * i + 1] << 8));
    interleaved_out[i] = float(v) / 32768.0f;
  }
  return true;
}

}  // namespace

bool has_inprocess_xma2_decoder() {
#ifdef SHIFT_HAVE_INPROCESS_XMA2
  return true;
#else
  return false;
#endif
}

bool decode_xma2_interleaved(const FsbSample& s, std::span<const uint8_t> raw_xma,
                             std::vector<float>& interleaved_out, int& out_channels, int& out_rate,
                             std::string* error) {
  interleaved_out.clear();
  out_channels = 0;
  out_rate = 0;
#ifndef SHIFT_HAVE_INPROCESS_XMA2
  (void)s;
  (void)raw_xma;
  if (error) *error = "in-process XMA2 decoder not enabled";
  return false;
#else
  static std::once_flag s_log_once;
  std::call_once(s_log_once, []() { av_log_set_level(AV_LOG_QUIET); });

  const int ch = std::clamp<int>(s.channels ? s.channels : 1, 1, 8);
  const int rate = s.rate > 0 ? s.rate : 48000;
  const size_t aligned_bytes = (raw_xma.size() / kXmaPacket) * kXmaPacket;
  if (aligned_bytes < kXmaPacket) {
    if (error) *error = "XMA2 payload shorter than one 2048-byte packet";
    return false;
  }

  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMA2);
  if (!codec) {
    if (error) *error = "AV_CODEC_ID_XMA2 decoder unavailable";
    return false;
  }

  AVCodecContext* ctx = avcodec_alloc_context3(codec);
  if (!ctx) {
    if (error) *error = "avcodec_alloc_context3 failed";
    return false;
  }

  // Build standard 34-byte XMA2WAVEFORMATEX extradata (matches xma2_wave() speaker masks).
  const uint16_t num_streams = uint16_t((ch + 1) / 2);
  uint32_t channel_mask = 0x3u;
  switch (ch) {
    case 1:  channel_mask = 0x4u; break;     // FC
    case 2:  channel_mask = 0x3u; break;     // FL | FR
    case 4:  channel_mask = 0x33u; break;    // FL | FR | BL | BR (quad)
    case 6:  channel_mask = 0x3Fu; break;    // 5.1 (FL|FR|FC|LFE|BL|BR)
    case 8:  channel_mask = 0x63Fu; break;   // 7.1
    default: channel_mask = (1u << ch) - 1u; break;
  }
  std::vector<uint8_t> extra;
  extra.reserve(34);
  put16_le(extra, num_streams);
  put32_le(extra, channel_mask);
  put32_le(extra, s.frames);
  put32_le(extra, uint32_t(raw_xma.size()));
  put32_le(extra, 0);         // play begin
  put32_le(extra, s.frames);  // play length
  put32_le(extra, 0);         // loop begin
  put32_le(extra, 0);         // loop length
  extra.push_back(0);         // loop count
  extra.push_back(4);         // encoder version
  put16_le(extra, 1);         // block count

  ctx->channels = ch;
  ctx->sample_rate = rate;
  ctx->block_align = int(kXmaPacket);
  ctx->bits_per_coded_sample = 16;
  ctx->extradata_size = int(extra.size());
  ctx->extradata = static_cast<uint8_t*>(av_mallocz(extra.size() + AV_INPUT_BUFFER_PADDING_SIZE));
  if (!ctx->extradata) {
    avcodec_free_context(&ctx);
    if (error) *error = "extradata allocation failed";
    return false;
  }
  std::memcpy(ctx->extradata, extra.data(), extra.size());

  // avcodec_open2 is not guaranteed thread-safe across concurrent threads in FFmpeg 4.x.
  static std::mutex s_open_mu;
  int open_rc = 0;
  {
    std::lock_guard<std::mutex> lk(s_open_mu);
    open_rc = avcodec_open2(ctx, codec, nullptr);
  }
  if (open_rc < 0) {
    avcodec_free_context(&ctx);
    if (error) *error = "avcodec_open2(XMA2) failed";
    return false;
  }

  AVPacket* pkt = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  if (!pkt || !frame) {
    if (pkt) av_packet_free(&pkt);
    if (frame) av_frame_free(&frame);
    avcodec_free_context(&ctx);
    if (error) *error = "packet/frame allocation failed";
    return false;
  }

  auto drain_frames = [&]() {
    while (avcodec_receive_frame(ctx, frame) == 0) {
      const int nb = frame->nb_samples;
      const int fch = std::max(1, frame->channels);
      const size_t base = interleaved_out.size();
      interleaved_out.resize(base + size_t(nb) * size_t(ch));
      if (frame->format == AV_SAMPLE_FMT_FLTP) {
        for (int i = 0; i < nb; ++i) {
          for (int c = 0; c < ch; ++c) {
            const int sc = std::min(c, fch - 1);
            const auto* plane = reinterpret_cast<const float*>(frame->extended_data[sc]);
            const float v = plane ? plane[i] : 0.0f;
            interleaved_out[base + size_t(i) * size_t(ch) + size_t(c)] = std::isfinite(v) ? std::clamp(v, -1.0f, 1.0f) : 0.0f;
          }
        }
      } else if (frame->format == AV_SAMPLE_FMT_S16P) {
        for (int i = 0; i < nb; ++i) {
          for (int c = 0; c < ch; ++c) {
            const int sc = std::min(c, fch - 1);
            const auto* plane = reinterpret_cast<const int16_t*>(frame->extended_data[sc]);
            interleaved_out[base + size_t(i) * size_t(ch) + size_t(c)] = plane ? (float(plane[i]) / 32768.0f) : 0.0f;
          }
        }
      } else if (frame->format == AV_SAMPLE_FMT_FLT) {
        const auto* src = reinterpret_cast<const float*>(frame->data[0]);
        for (int i = 0; i < nb; ++i) {
          for (int c = 0; c < ch; ++c) {
            const int sc = std::min(c, fch - 1);
            const float v = src ? src[size_t(i) * size_t(fch) + size_t(sc)] : 0.0f;
            interleaved_out[base + size_t(i) * size_t(ch) + size_t(c)] = std::isfinite(v) ? std::clamp(v, -1.0f, 1.0f) : 0.0f;
          }
        }
      } else if (frame->format == AV_SAMPLE_FMT_S16) {
        const auto* src = reinterpret_cast<const int16_t*>(frame->data[0]);
        for (int i = 0; i < nb; ++i) {
          for (int c = 0; c < ch; ++c) {
            const int sc = std::min(c, fch - 1);
            interleaved_out[base + size_t(i) * size_t(ch) + size_t(c)] =
                src ? (float(src[size_t(i) * size_t(fch) + size_t(sc)]) / 32768.0f) : 0.0f;
          }
        }
      }
      av_frame_unref(frame);
    }
  };

  // Cap reserve/resize by a sane bound derived from decoded payload size (XMA2 max ~25:1 compression
  // plus 64K frames slack) so malformed FSB headers cannot trigger multi-GB OOM allocations.
  constexpr size_t kMaxSaneFrames = 48000ull * 900ull;  // 15 min @ 48 kHz
  const size_t max_expected_frames = std::min<size_t>(kMaxSaneFrames, aligned_bytes * 16ull + 65536ull);
  if (s.frames > 0 && size_t(s.frames) <= max_expected_frames) {
    interleaved_out.reserve(size_t(s.frames + 4096) * size_t(ch));
  }

  for (size_t off = 0; off + kXmaPacket <= aligned_bytes; off += kXmaPacket) {
    if (av_new_packet(pkt, int(kXmaPacket)) < 0) break;
    std::memcpy(pkt->data, raw_xma.data() + off, kXmaPacket);
    int eagain_retries = 0;
    for (;;) {
      const int sr = avcodec_send_packet(ctx, pkt);
      if (sr == AVERROR(EAGAIN)) {
        const size_t before = interleaved_out.size();
        drain_frames();
        if (interleaved_out.size() == before && ++eagain_retries > 16) break;
        continue;
      }
      if (sr >= 0) drain_frames();
      break;
    }
    av_packet_unref(pkt);
  }
  avcodec_send_packet(ctx, nullptr);
  drain_frames();

  av_frame_free(&frame);
  av_packet_free(&pkt);
  avcodec_free_context(&ctx);

  if (interleaved_out.empty()) {
    if (error) *error = "XMA2 decoder produced 0 frames";
    return false;
  }
  if (s.frames > 0 && size_t(s.frames) <= max_expected_frames) {
    const size_t target = size_t(s.frames) * size_t(ch);
    interleaved_out.resize(target, 0.0f);
  }
  out_channels = ch;
  out_rate = rate;
  return true;
#endif
}

bool decode_xma2_mono(const FsbSample& s, std::span<const uint8_t> raw_xma, PcmSample& out, std::string* error) {
  std::vector<float> inter;
  int ch = 0, rate = 0;
  if (!decode_xma2_interleaved(s, raw_xma, inter, ch, rate, error)) return false;
  const size_t frames = inter.size() / size_t(std::max(1, ch));
  out.rate = rate;
  out.data.resize(frames);
  if (ch == 1) {
    std::copy_n(inter.data(), frames, out.data.data());
  } else {
    const float inv = 1.0f / float(ch);
    for (size_t i = 0; i < frames; ++i) {
      float acc = 0.0f;
      for (int c = 0; c < ch; ++c) acc += inter[i * size_t(ch) + size_t(c)];
      out.data[i] = acc * inv;
    }
  }
  return !out.data.empty();
}

bool decode_fsb_subsound_mono(const std::filesystem::path& bank_path, uint32_t subsound, PcmSample& out,
                              const std::filesystem::path& ffmpeg_fallback, const std::filesystem::path& temp_dir) {
  FsbSample s{};
  if (!get_cached_fsb4_sample(bank_path, subsound, s) || s.bytes == 0) return false;
  const int rate = s.rate > 0 ? s.rate : 48000;
  if (s.xma()) {
    std::vector<uint8_t> clip_bytes;
    if (!read_file_range(bank_path, s.offset, s.bytes, clip_bytes)) return false;
    const auto span = std::span<const uint8_t>(clip_bytes);
    if (decode_xma2_mono(s, span, out, nullptr)) return true;
    std::vector<float> inter;
    int ch = 0, dr = 0;
    if (!decode_xma2_via_cli(s, span, ffmpeg_fallback, temp_dir, inter, ch, dr)) return false;
    const size_t frames = inter.size() / size_t(std::max(1, ch));
    out.rate = dr;
    out.data.resize(frames);
    const float inv = 1.0f / float(std::max(1, ch));
    for (size_t i = 0; i < frames; ++i) {
      float acc = 0.0f;
      for (int c = 0; c < ch; ++c) acc += inter[i * size_t(ch) + size_t(c)];
      out.data[i] = acc * inv;
    }
    return !out.data.empty();
  }
  auto src = make_fsb_mpeg_source(bank_path, s.offset, s.bytes, rate);
  if (src && src->open()) {
    out.rate = src->rate();
    out.data.clear();
    float tmp[2048 * 2];
    while (out.data.size() < size_t(out.rate) * 60) {
      const int64_t got = src->read(tmp, 2048);
      if (got <= 0) break;
      for (int64_t i = 0; i < got; ++i) out.data.push_back(0.5f * (tmp[2 * i] + tmp[2 * i + 1]));
    }
    return !out.data.empty();
  }
  return false;
}

bool decode_fsb_subsound_stereo(const std::filesystem::path& bank_path, uint32_t subsound,
                                std::vector<float>& stereo_interleaved, int& out_rate,
                                const std::filesystem::path& ffmpeg_fallback, const std::filesystem::path& temp_dir) {
  stereo_interleaved.clear();
  out_rate = 0;
  FsbSample s{};
  if (!get_cached_fsb4_sample(bank_path, subsound, s) || s.bytes == 0) return false;
  const int rate = s.rate > 0 ? s.rate : 48000;
  if (s.xma()) {
    std::vector<uint8_t> clip_bytes;
    if (!read_file_range(bank_path, s.offset, s.bytes, clip_bytes)) return false;
    const auto span = std::span<const uint8_t>(clip_bytes);
    std::vector<float> inter;
    int ch = 0, dr = 0;
    if (!decode_xma2_interleaved(s, span, inter, ch, dr, nullptr) &&
        !decode_xma2_via_cli(s, span, ffmpeg_fallback, temp_dir, inter, ch, dr)) {
      return false;
    }
    const size_t frames = inter.size() / size_t(std::max(1, ch));
    out_rate = dr;
    if (ch == 2) {
      stereo_interleaved = std::move(inter);
    } else if (ch == 1) {
      stereo_interleaved.resize(frames * 2);
      for (size_t i = 0; i < frames; ++i) {
        stereo_interleaved[2 * i] = inter[i];
        stereo_interleaved[2 * i + 1] = inter[i];
      }
    } else if (ch >= 6) {
      // Standard 5.1 -> stereo fold (FL, FR, C, LFE, RL, RR).
      stereo_interleaved.resize(frames * 2);
      constexpr float kFold = 0.70710678f;
      for (size_t i = 0; i < frames; ++i) {
        const float* f = &inter[i * size_t(ch)];
        stereo_interleaved[2 * i] = std::clamp(f[0] + kFold * f[2] + kFold * f[4], -1.0f, 1.0f);
        stereo_interleaved[2 * i + 1] = std::clamp(f[1] + kFold * f[2] + kFold * f[5], -1.0f, 1.0f);
      }
    } else {
      stereo_interleaved.resize(frames * 2);
      for (size_t i = 0; i < frames; ++i) {
        stereo_interleaved[2 * i] = inter[i * size_t(ch)];
        stereo_interleaved[2 * i + 1] = inter[i * size_t(ch) + 1];
      }
    }
    return !stereo_interleaved.empty();
  }
  auto src = make_fsb_mpeg_source(bank_path, s.offset, s.bytes, rate);
  if (src && src->open()) {
    out_rate = src->rate();
    float tmp[2048 * 2];
    while (stereo_interleaved.size() < size_t(out_rate) * 600 * 2) {
      const int64_t got = src->read(tmp, 2048);
      if (got <= 0) break;
      stereo_interleaved.insert(stereo_interleaved.end(), tmp, tmp + size_t(got) * 2);
    }
    return !stereo_interleaved.empty();
  }
  return false;
}

}  // namespace audio
