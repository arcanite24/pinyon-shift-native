#include "audio/fsb4.h"

#include <cstring>
#include <fstream>
#include <system_error>

namespace audio {
namespace {

constexpr size_t kBankHeader = 48, kSampleHeader = 80, kAlign = 32, kXmaPacket = 2048;

uint16_t u16(std::span<const uint8_t> b, size_t at) { return uint16_t(b[at] | b[at + 1] << 8); }
uint32_t u32(std::span<const uint8_t> b, size_t at) {
  return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24;
}

void put16(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(uint8_t(x)), v.push_back(uint8_t(x >> 8));
}
void put32(std::vector<uint8_t>& v, uint32_t x) {
  for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}
void tag(std::vector<uint8_t>& v, const char* t) { v.insert(v.end(), t, t + 4); }

}  // namespace

bool read_fsb4(std::span<const uint8_t> bank, std::vector<FsbSample>& out, std::string* error, uint64_t bank_size) {
  auto fail = [&](const char* what) {
    if (error) *error = what;
    return false;
  };
  out.clear();
  if (bank.size() < kBankHeader || std::memcmp(bank.data(), "FSB4", 4) != 0) return fail("not an FSB4 bank");
  const uint32_t count = u32(bank, 4), header_bytes = u32(bank, 8);
  if (kBankHeader + uint64_t(header_bytes) > bank.size()) return fail("sample headers past the end");
  uint64_t offset = (kBankHeader + uint64_t(header_bytes) + kAlign - 1) & ~uint64_t(kAlign - 1);
  size_t at = kBankHeader;
  const size_t end = kBankHeader + header_bytes;
  for (uint32_t i = 0; i < count; ++i) {
    if (at + kSampleHeader > end) return fail("sample header past the header block");
    const uint16_t record = u16(bank, at);
    if (record < kSampleHeader) return fail("short sample header");
    FsbSample s;
    const char* name = reinterpret_cast<const char*>(bank.data() + at + 2);
    s.name.assign(name, strnlen(name, 30));
    s.frames = u32(bank, at + 32);
    s.bytes = u32(bank, at + 36);
    s.mode = u32(bank, at + 48);
    s.rate = int32_t(u32(bank, at + 52));
    s.channels = u16(bank, at + 62);
    s.offset = offset;
    if (offset + s.bytes > (bank_size ? bank_size : bank.size())) return fail("sample data past the end of the bank");
    out.push_back(std::move(s));
    offset = (offset + out.back().bytes + kAlign - 1) & ~uint64_t(kAlign - 1);
    at += record;
  }
  return true;
}

bool read_fsb4_file_headers(const std::filesystem::path& path, std::vector<FsbSample>& out, std::string* error) {
  auto fail = [&](const char* what) {
    if (error) *error = what;
    return false;
  };
  out.clear();
  std::error_code ec;
  const uint64_t file_size = std::filesystem::file_size(path, ec);
  if (ec || file_size < kBankHeader) return fail("cannot stat FSB4 bank");
  std::ifstream f(path, std::ios::binary);
  if (!f) return fail("cannot open FSB4 bank");
  uint8_t head[kBankHeader];
  if (!f.read(reinterpret_cast<char*>(head), kBankHeader)) return fail("cannot read FSB4 header");
  if (std::memcmp(head, "FSB4", 4) != 0) return fail("not an FSB4 bank");
  const uint32_t header_bytes = u32(head, 8);
  constexpr uint32_t kMaxHeaderBytes = 16u * 1024u * 1024u;
  if (header_bytes > kMaxHeaderBytes || kBankHeader + uint64_t(header_bytes) > file_size) {
    return fail("sample headers past the end");
  }
  std::vector<uint8_t> buf(kBankHeader + size_t(header_bytes));
  std::memcpy(buf.data(), head, kBankHeader);
  if (header_bytes > 0 && !f.read(reinterpret_cast<char*>(buf.data() + kBankHeader), header_bytes)) {
    return fail("cannot read FSB4 sample headers");
  }
  return read_fsb4(buf, out, error, file_size);
}

std::vector<uint8_t> xma2_wave(const FsbSample& s, std::span<const uint8_t> data) {
  const uint32_t channels = s.channels ? s.channels : 1;
  const uint32_t num_streams = (channels + 1u) / 2u;
  uint32_t mask = 0x4u;
  if (channels == 1) mask = 0x4u;
  else if (channels == 2) mask = 0x3u;
  else if (channels == 4) mask = 0x33u;
  else if (channels == 6) mask = 0x3Fu;
  else if (channels == 8) mask = 0x63Fu;
  else mask = (channels < 32u) ? ((1u << channels) - 1u) : 0xFFFFFFFFu;
  const size_t body = data.size() / kXmaPacket * kXmaPacket;
  std::vector<uint8_t> extra;
  put16(extra, num_streams);  // streams ((channels + 1) / 2 stereo pairs)
  put32(extra, mask);         // channel mask
  put32(extra, s.frames);   // samples encoded
  put32(extra, uint32_t(data.size()));  // bytes per block
  put32(extra, 0);          // play begin
  put32(extra, s.frames);   // play length
  put32(extra, 0);          // loop begin
  put32(extra, 0);          // loop length
  extra.push_back(0);       // loop count
  extra.push_back(4);       // encoder version
  put16(extra, 1);          // block count
  std::vector<uint8_t> fmt;
  put16(fmt, 0x166);
  put16(fmt, channels);
  put32(fmt, uint32_t(s.rate));
  put32(fmt, uint32_t(s.rate) * channels * 2);
  put16(fmt, uint32_t(kXmaPacket));
  put16(fmt, 16);
  put16(fmt, uint32_t(extra.size()));
  fmt.insert(fmt.end(), extra.begin(), extra.end());
  std::vector<uint8_t> w;
  tag(w, "RIFF");
  put32(w, uint32_t(4 + 8 + fmt.size() + 8 + body));
  tag(w, "WAVE");
  tag(w, "fmt ");
  put32(w, uint32_t(fmt.size()));
  w.insert(w.end(), fmt.begin(), fmt.end());
  tag(w, "data");
  put32(w, uint32_t(body));
  w.insert(w.end(), data.begin(), data.begin() + long(body));
  return w;
}

}  // namespace audio
