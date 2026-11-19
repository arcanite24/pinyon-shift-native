// XMem LZX streams (ZIP method 21).
//
// The compressed stream is a sequence of frames, one per 32 KiB of output.
// A frame starts with a big-endian u16 compressed size; a frame that is
// shorter than 32 KiB of output starts with 0xFF, then a big-endian u16
// output size and a u16 compressed size. The frame bodies, concatenated,
// are one LZX bitstream (decoded with libmspack's lzxd). The window is
// 64 KiB on the files seen so far; 128 and 32 KiB are tried as fallbacks.
// Spec: site/findings/files.toml; framing as in horizon1-recomp's
// src/ui/hostui/fh1_archive.cpp ParseXmemPayload.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace fh1 {

// Strips the XMem frame headers, leaving the LZX bitstream.
bool xmem_unframe(std::span<const uint8_t> compressed, size_t uncompressed_size,
                  std::vector<uint8_t>& lzx);

// One raw LZX decode with the given window (bits 15-21). Returns false on a
// corrupt stream or a short output.
bool lzx_decode(std::span<const uint8_t> lzx, std::span<uint8_t> output, int window_bits);

// Unframes and decodes an XMem member. When `expected_crc` is given, a
// window size is accepted only if the output matches it.
std::optional<std::vector<uint8_t>> xmem_decompress(std::span<const uint8_t> compressed,
                                                    size_t uncompressed_size,
                                                    std::optional<uint32_t> expected_crc = {});

}  // namespace fh1
