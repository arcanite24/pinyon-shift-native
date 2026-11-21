// FSB4 sound banks (little-endian): a 48-byte bank header, one sample
// header per sample (an 80-byte base and an extra block), then the sample
// data, each sample 32-byte aligned. Engine banks hold XMA only.
// Spec: SPEC-ENGINE-SOUND "Audio format"; site/findings/audio.toml
// "FSB4 sample header layout".
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace audio {

struct FsbSample {
  std::string name;
  uint32_t frames = 0;      // length in samples per channel
  uint32_t bytes = 0;       // compressed bytes
  uint32_t mode = 0;
  int32_t rate = 0;         // default frequency, Hz
  uint16_t channels = 1;
  uint64_t offset = 0;      // of the data in the bank
  bool xma() const { return (mode & 0x01000000u) != 0; }
};

// `bank_size`: the whole bank's size when `bank` is only its headers (a
// large bank read in part); 0: `bank` is the whole bank.
bool read_fsb4(std::span<const uint8_t> bank, std::vector<FsbSample>& out, std::string* error,
               uint64_t bank_size = 0);

// A RIFF WAVE with an XMA2WAVEFORMATEX header (format 0x166, one stream,
// 2048-byte packets) around one sample's packets, which FFmpeg's xma2
// decoder reads (the RI-0.6 decode check's wrapping).
std::vector<uint8_t> xma2_wave(const FsbSample& sample, std::span<const uint8_t> data);

}  // namespace audio
