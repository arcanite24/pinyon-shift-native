// Spec: SPEC-EVENT-RESTRICTIONS (horizon1-recomp docs/specs/EVENT_RESTRICTIONS.md).
#include "data/event_restrictions.h"

#include <cstddef>
#include <vector>

namespace shift::data {
namespace {

constexpr int kMaskBits = 40;
constexpr size_t kMaskBytes = 5;

// Width in bytes of each bit the spec gives; -1 = unknown (rejected).
// Width 0 means the bit alone is the restriction.
constexpr std::array<int, kMaskBits> kWidths = [] {
  std::array<int, kMaskBits> w{};
  for (int& x : w) x = -1;
  w[restriction_bit::kYearMin] = 4;
  w[restriction_bit::kYearMax] = 4;
  w[restriction_bit::kBodyFamily] = 1;
  w[restriction_bit::kRegion] = 1;
  w[restriction_bit::kCountry] = 1;
  w[restriction_bit::kMake] = 2;
  w[restriction_bit::kModelFamily] = 2;
  w[restriction_bit::kDriveType] = 1;
  w[restriction_bit::kFlag34] = 0;
  w[restriction_bit::kValueMin] = 4;
  w[restriction_bit::kValueMax] = 4;
  w[restriction_bit::kRallyUpgraded] = 0;
  return w;
}();

int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

// Field values indexed by bit; nullopt = bit not set.
using Fields = std::array<std::optional<uint32_t>, kMaskBits>;

Fields to_fields(const RestrictionSet& s) {
  namespace b = restriction_bit;
  Fields f{};
  f[b::kYearMin] = s.year_min;
  f[b::kYearMax] = s.year_max;
  if (s.body_family) f[b::kBodyFamily] = *s.body_family;
  if (s.region) f[b::kRegion] = *s.region;
  if (s.country) f[b::kCountry] = *s.country;
  if (s.make) f[b::kMake] = *s.make;
  if (s.model_family) f[b::kModelFamily] = *s.model_family;
  if (s.drive_type) f[b::kDriveType] = *s.drive_type;
  if (s.flag34) f[b::kFlag34] = 0;
  f[b::kValueMin] = s.value_min;
  f[b::kValueMax] = s.value_max;
  if (s.rally_upgraded) f[b::kRallyUpgraded] = 0;
  return f;
}

RestrictionSet from_fields(const Fields& f) {
  namespace b = restriction_bit;
  RestrictionSet s;
  s.year_min = f[b::kYearMin];
  s.year_max = f[b::kYearMax];
  if (f[b::kBodyFamily]) s.body_family = uint8_t(*f[b::kBodyFamily]);
  if (f[b::kRegion]) s.region = uint8_t(*f[b::kRegion]);
  if (f[b::kCountry]) s.country = uint8_t(*f[b::kCountry]);
  if (f[b::kMake]) s.make = uint16_t(*f[b::kMake]);
  if (f[b::kModelFamily]) s.model_family = uint16_t(*f[b::kModelFamily]);
  if (f[b::kDriveType]) s.drive_type = uint8_t(*f[b::kDriveType]);
  s.flag34 = f[b::kFlag34].has_value();
  s.value_min = f[b::kValueMin];
  s.value_max = f[b::kValueMax];
  s.rally_upgraded = f[b::kRallyUpgraded].has_value();
  return s;
}

RestrictionDecode fail(RestrictionError error, int bit = -1) {
  RestrictionDecode d;
  d.error = error;
  d.bit = bit;
  return d;  // d.set stays empty: a rejected value means "no restrictions"
}

}  // namespace

bool RestrictionSet::empty() const { return *this == RestrictionSet{}; }

const char* to_string(RestrictionError error) {
  switch (error) {
    case RestrictionError::kNone: return "ok";
    case RestrictionError::kOddLength: return "odd number of digits";
    case RestrictionError::kTooShort: return "fewer than 10 digits";
    case RestrictionError::kNotHex: return "non-hexadecimal character";
    case RestrictionError::kUnknownBit: return "bit without a known width";
    case RestrictionError::kTruncated: return "field runs past the end";
    case RestrictionError::kTrailingBytes: return "bytes left after the last field";
  }
  return "unknown error";
}

RestrictionDecode decode_restriction_value(std::string_view value) {
  // Odd length is checked first, so a 9-digit value reports "odd length"
  // as the spec's test table does.
  if (value.size() % 2) return fail(RestrictionError::kOddLength);
  if (value.size() < kMaskBytes * 2) return fail(RestrictionError::kTooShort);

  std::vector<uint8_t> bytes(value.size() / 2);
  for (size_t i = 0; i < bytes.size(); ++i) {
    const int hi = hex_digit(value[2 * i]);
    const int lo = hex_digit(value[2 * i + 1]);
    if (hi < 0 || lo < 0) return fail(RestrictionError::kNotHex);
    bytes[i] = uint8_t(hi << 4 | lo);
  }

  uint64_t mask = 0;
  for (size_t i = 0; i < kMaskBytes; ++i) mask |= uint64_t(bytes[i]) << (8 * i);

  // Reject an unknown bit before reading any field, so the error does not
  // depend on where the fields end.
  for (int bit = 0; bit < kMaskBits; ++bit)
    if ((mask >> bit & 1) && kWidths[size_t(bit)] < 0) return fail(RestrictionError::kUnknownBit, bit);

  Fields fields{};
  size_t pos = kMaskBytes;
  for (int bit = 0; bit < kMaskBits; ++bit) {
    if (!(mask >> bit & 1)) continue;
    const size_t width = size_t(kWidths[size_t(bit)]);
    if (bytes.size() - pos < width) return fail(RestrictionError::kTruncated, bit);
    uint32_t v = 0;
    for (size_t i = 0; i < width; ++i) v = v << 8 | bytes[pos + i];  // big-endian
    pos += width;
    fields[size_t(bit)] = v;
  }
  if (pos != bytes.size()) return fail(RestrictionError::kTrailingBytes);

  RestrictionDecode d;
  d.set = from_fields(fields);
  return d;
}

std::string encode_restriction_value(const RestrictionSet& set) {
  static constexpr char kDigits[] = "0123456789ABCDEF";
  const Fields fields = to_fields(set);
  uint64_t mask = 0;
  for (int bit = 0; bit < kMaskBits; ++bit)
    if (fields[size_t(bit)]) mask |= uint64_t(1) << bit;

  std::string out;
  auto put = [&](uint8_t byte) {
    out += kDigits[byte >> 4];
    out += kDigits[byte & 15];
  };
  for (size_t i = 0; i < kMaskBytes; ++i) put(uint8_t(mask >> (8 * i)));
  for (int bit = 0; bit < kMaskBits; ++bit) {
    if (!fields[size_t(bit)]) continue;
    const int width = kWidths[size_t(bit)];
    for (int i = width - 1; i >= 0; --i) put(uint8_t(*fields[size_t(bit)] >> (8 * i)));
  }
  return out;
}

bool car_passes(const RestrictionSet& s, const CarProperties& car) {
  if (s.year_min && car.year < *s.year_min) return false;
  if (s.year_max && car.year > *s.year_max) return false;
  if (s.value_min && car.value < *s.value_min) return false;
  if (s.value_max && car.value > *s.value_max) return false;
  if (s.body_family && car.body_family != *s.body_family) return false;
  if (s.region && car.region != *s.region) return false;
  if (s.country && car.country != *s.country) return false;
  if (s.make && car.make != *s.make) return false;
  if (s.model_family && car.model_family != *s.model_family) return false;
  if (s.drive_type && car.drive_type != *s.drive_type) return false;
  if (s.rally_upgraded && !car.rally_upgraded) return false;
  // flag34 has no known effect (spec: clearing it changed nothing seen).
  return true;
}

BucketError set_restriction_bucket(EventRestrictions& event, int64_t bucket_id,
                                   std::string_view value, RestrictionDecode* decode) {
  RestrictionDecode d = decode_restriction_value(value);
  if (decode) *decode = d;
  // The spec gives buckets 0 to 3 only; any other ID is refused rather
  // than mapped (open question for the specification side).
  if (bucket_id < 0 || bucket_id >= kRestrictionBuckets) return BucketError::kOutOfRange;
  event.buckets[size_t(bucket_id)] = d.set;  // empty on a rejected value
  return d.ok() ? BucketError::kNone : BucketError::kBadValue;
}

}  // namespace shift::data
