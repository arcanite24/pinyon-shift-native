// Event restriction values: which cars may enter an event.
//
// The game database's EventRestrictions table holds one hexadecimal Value
// string per (event, bucket). The string is a 40-bit presence mask, least
// significant byte first, followed by one big-endian field per set bit in
// ascending bit order. Each bit has a fixed width; bits whose width the
// spec does not give are rejected rather than guessed.
//
// Spec: SPEC-EVENT-RESTRICTIONS (horizon1-recomp docs/specs/EVENT_RESTRICTIONS.md).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace shift::data {

// Bit numbers of the mask, as the spec's table names them.
namespace restriction_bit {
inline constexpr int kYearMin = 8;
inline constexpr int kYearMax = 9;
inline constexpr int kBodyFamily = 11;
inline constexpr int kRegion = 15;
inline constexpr int kCountry = 17;
inline constexpr int kMake = 19;
inline constexpr int kModelFamily = 21;
inline constexpr int kDriveType = 25;
inline constexpr int kFlag34 = 34;
inline constexpr int kValueMin = 37;
inline constexpr int kValueMax = 38;
}  // namespace restriction_bit

// Drive type IDs (List_DriveType).
inline constexpr uint8_t kDriveFwd = 1;
inline constexpr uint8_t kDriveRwd = 2;
inline constexpr uint8_t kDriveAwd = 3;

// One bucket's restrictions. An absent field sets no limit; a set with no
// fields admits every car. Bounds are inclusive.
struct RestrictionSet {
  std::optional<uint32_t> year_min;      // bit 8, 4 bytes
  std::optional<uint32_t> year_max;      // bit 9, 4 bytes
  std::optional<uint8_t> body_family;    // bit 11, 1 byte (List_FamilyBody.Id)
  std::optional<uint8_t> region;         // bit 15, 1 byte (List_Region.Id)
  std::optional<uint8_t> country;        // bit 17, 1 byte (List_Country.CountryID)
  std::optional<uint16_t> make;          // bit 19, 2 bytes (List_CarMake.ID)
  std::optional<uint16_t> model_family;  // bit 21, 2 bytes (List_FamilyModel.Id)
  std::optional<uint8_t> drive_type;     // bit 25, 1 byte (List_DriveType.ID)
  bool flag34 = false;                   // bit 34, no value; no known effect
  std::optional<uint32_t> value_min;     // bit 37, 4 bytes, credits
  std::optional<uint32_t> value_max;     // bit 38, 4 bytes, credits

  bool empty() const;
  bool operator==(const RestrictionSet&) const = default;
};

enum class RestrictionError {
  kNone,
  kOddLength,      // odd number of digits
  kTooShort,       // fewer than 10 digits (the mask alone)
  kNotHex,         // a character that is not a hexadecimal digit
  kUnknownBit,     // a set bit whose width the spec does not give
  kTruncated,      // a field runs past the end of the string
  kTrailingBytes,  // bytes left after the last field
};

const char* to_string(RestrictionError error);

struct RestrictionDecode {
  // On any error this is the empty set ("no restrictions"), never a partly
  // decoded one; the caller logs the error with the event ID.
  RestrictionSet set;
  RestrictionError error = RestrictionError::kNone;
  int bit = -1;  // kUnknownBit: the lowest offending bit; kTruncated: the field's bit

  bool ok() const { return error == RestrictionError::kNone; }
};

// Decodes a Value string. Accepts upper- and lower-case hex digits.
RestrictionDecode decode_restriction_value(std::string_view value);

// Encodes a set back to a Value string: the mask, then each present field,
// in upper-case hex digits.
std::string encode_restriction_value(const RestrictionSet& set);

// The car properties the restrictions test.
struct CarProperties {
  uint32_t year = 0;   // model year
  uint32_t value = 0;  // price in credits
  uint8_t body_family = 0;
  uint8_t region = 0;
  uint8_t country = 0;
  uint16_t make = 0;
  uint16_t model_family = 0;
  uint8_t drive_type = 0;
};

// True when the car satisfies every field present in the set.
bool car_passes(const RestrictionSet& set, const CarProperties& car);

// An event keeps four buckets (BucketId 0 to 3). The disc only uses bucket
// 0. How buckets combine is not specified, so this type only stores them;
// callers test one bucket with car_passes.
inline constexpr int kRestrictionBuckets = 4;

struct EventRestrictions {
  std::array<RestrictionSet, kRestrictionBuckets> buckets{};
};

enum class BucketError { kNone, kOutOfRange, kBadValue };

// Decodes `value` into `event.buckets[bucket_id]`. A bucket outside 0..3 is
// refused and leaves the event unchanged; a rejected value stores the empty
// set. `decode` (optional) receives the value's decode result.
BucketError set_restriction_bucket(EventRestrictions& event, int64_t bucket_id,
                                   std::string_view value, RestrictionDecode* decode = nullptr);

}  // namespace shift::data
