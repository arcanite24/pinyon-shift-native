// In-process XMA2 (Xbox Media Audio 2) decoder backed by LGPL FFmpeg libavcodec
// (wmaprodec / ff_xma2_decoder) when built with SHIFT_EMBED_XMA2=ON.
// Eliminates external `ffmpeg` CLI process spawning and temp files across
// Windows, Linux, macOS, and Android.
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "audio/engine_player.h"
#include "audio/fsb4.h"

namespace audio {

// Returns true when the in-process XMA2 decoder is compiled in.
bool has_inprocess_xma2_decoder();

// Decodes one FSB4 XMA2 subsound directly from memory into interleaved float32
// PCM (`[-1, 1]`, `out_channels` channels at `out_rate` Hz).
// If `s.frames > 0`, trims or zero-pads to the exact declared sample count.
bool decode_xma2_interleaved(const FsbSample& s, std::span<const uint8_t> raw_xma,
                             std::vector<float>& interleaved_out, int& out_channels, int& out_rate,
                             std::string* error = nullptr);

// Convenience wrapper that mixes all channels down to mono `PcmSample`.
bool decode_xma2_mono(const FsbSample& s, std::span<const uint8_t> raw_xma, PcmSample& out,
                      std::string* error = nullptr);

// Decodes one FSB4 subsound (XMA2 in-process, or MPEG via dr_mp3) into mono
// `PcmSample`. Falls back to `ffmpeg_fallback` CLI only when in-process XMA2
// is disabled.
bool decode_fsb_subsound_mono(const std::filesystem::path& bank_path, uint32_t subsound, PcmSample& out,
                              const std::filesystem::path& ffmpeg_fallback = {},
                              const std::filesystem::path& temp_dir = {});

// Decodes one FSB4 subsound (XMA2 in-process, or MPEG via dr_mp3) into stereo
// interleaved float32 PCM (`2 * frames` floats).
bool decode_fsb_subsound_stereo(const std::filesystem::path& bank_path, uint32_t subsound,
                                std::vector<float>& stereo_interleaved, int& out_rate,
                                const std::filesystem::path& ffmpeg_fallback = {},
                                const std::filesystem::path& temp_dir = {});

}  // namespace audio
