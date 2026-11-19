// Unit tests for libs/data on synthetic values only (no game data).
// Spec: SPEC-EVENT-RESTRICTIONS, section "Tests".
#include <cstdio>
#include <string>

#include "data/event_restrictions.h"

namespace {

int failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      ++failures;                                                      \
    }                                                                  \
  } while (0)

using namespace shift::data;

void check_round_trip(const std::string& value) {
  const auto d = decode_restriction_value(value);
  CHECK(d.ok());
  CHECK(encode_restriction_value(d.set) == value);
}

void test_spec_valid_cases() {
  {
    const auto d = decode_restriction_value("0000000000");
    CHECK(d.ok());
    CHECK(d.set.empty());
  }
  {
    const auto d = decode_restriction_value("0000000004");
    CHECK(d.ok());
    RestrictionSet want;
    want.flag34 = true;
    CHECK(d.set == want);
  }
  {
    const auto d = decode_restriction_value("0003000004000007BC000007C5");
    CHECK(d.ok());
    RestrictionSet want;
    want.year_min = 1980;
    want.year_max = 1989;
    want.flag34 = true;
    CHECK(d.set == want);
  }
  {
    const auto d = decode_restriction_value("0000080004000D");
    CHECK(d.ok());
    RestrictionSet want;
    want.make = 13;
    want.flag34 = true;
    CHECK(d.set == want);
  }
  {
    const auto d = decode_restriction_value("000000020403");
    CHECK(d.ok());
    RestrictionSet want;
    want.drive_type = kDriveAwd;
    want.flag34 = true;
    CHECK(d.set == want);
  }
  {
    const auto d = decode_restriction_value("00000000640000000000003A98");
    CHECK(d.ok());
    RestrictionSet want;
    want.value_min = 0;  // present and zero, distinct from absent
    want.value_max = 15000;
    want.flag34 = true;
    CHECK(d.set == want);
  }
}

void test_spec_rejected_cases() {
  CHECK(decode_restriction_value("00000000").error == RestrictionError::kTooShort);
  CHECK(decode_restriction_value("000000000").error == RestrictionError::kOddLength);
  {
    const auto d = decode_restriction_value("0000000001");
    CHECK(d.error == RestrictionError::kUnknownBit);
    CHECK(d.bit == 32);
    CHECK(d.set.empty());
  }
  {
    const auto d = decode_restriction_value("000008000400");
    CHECK(d.error == RestrictionError::kTruncated);
    CHECK(d.bit == restriction_bit::kMake);
    CHECK(d.set.empty());
  }
}

void test_other_errors() {
  CHECK(decode_restriction_value("").error == RestrictionError::kTooShort);
  CHECK(decode_restriction_value("00000000G0").error == RestrictionError::kNotHex);
  CHECK(decode_restriction_value("0000 00000").error == RestrictionError::kNotHex);
  // Leftover bytes after the last field.
  CHECK(decode_restriction_value("000000000000").error == RestrictionError::kTrailingBytes);
  CHECK(decode_restriction_value("000000020403FF").error == RestrictionError::kTrailingBytes);
  // Every unknown bit is refused, including even pair bits and bits above 34.
  for (int bit = 0; bit < 40; ++bit) {
    const bool known = bit == 8 || bit == 9 || bit == 11 || bit == 15 || bit == 17 || bit == 19 ||
                       bit == 21 || bit == 25 || bit == 34 || bit == 37 || bit == 38;
    if (known) continue;
    RestrictionSet empty;
    std::string v = encode_restriction_value(empty);
    const int byte = bit / 8;
    const char digits[] = "0123456789ABCDEF";
    const int b = 1 << (bit % 8);
    v[size_t(2 * byte)] = digits[b >> 4];
    v[size_t(2 * byte + 1)] = digits[b & 15];
    const auto d = decode_restriction_value(v);
    CHECK(d.error == RestrictionError::kUnknownBit && d.bit == bit);
  }
  // An unknown bit wins over a truncation that comes earlier in the fields.
  CHECK(decode_restriction_value("0003000001").error == RestrictionError::kUnknownBit);
  // All or nothing: a valid year min followed by a truncated year max.
  {
    const auto d = decode_restriction_value("0003000000000007BC0007");
    CHECK(d.error == RestrictionError::kTruncated);
    CHECK(d.bit == restriction_bit::kYearMax);
    CHECK(d.set.empty());
  }
  // Lower-case digits decode; the encoder writes upper case.
  {
    const auto d = decode_restriction_value("0000080004000d");
    CHECK(d.ok() && d.set.make == 13);
    CHECK(encode_restriction_value(d.set) == "0000080004000D");
  }
}

void test_round_trip() {
  check_round_trip("0000000000");
  check_round_trip("0000000004");
  check_round_trip("0003000004000007BC000007C5");
  check_round_trip("0000080004000D");
  check_round_trip("000000020403");
  check_round_trip("00000000640000000000003A98");
  // Every known field at once, with values that use each byte.
  RestrictionSet all;
  all.year_min = 0x01020304;
  all.year_max = 0xFFFFFFFF;
  all.body_family = 0x81;
  all.region = 0x7F;
  all.country = 0x00;
  all.make = 0xBEEF;
  all.model_family = 0x0102;
  all.drive_type = kDriveRwd;
  all.flag34 = true;
  all.value_min = 0;
  all.value_max = 0x80000000u;
  const std::string encoded = encode_restriction_value(all);
  // Mask bytes LSB first: bits 8,9,11,15 / 17,19,21 / 25 / 34,37,38.
  CHECK(encoded.substr(0, 10) == "008B2A0264");
  const auto d = decode_restriction_value(encoded);
  CHECK(d.ok());
  CHECK(d.set == all);
  check_round_trip(encoded);
}

void test_predicate() {
  CarProperties car;
  car.year = 1985;
  car.value = 15000;
  car.body_family = 4;
  car.region = 2;
  car.country = 7;
  car.make = 13;
  car.model_family = 300;
  car.drive_type = kDriveAwd;

  CHECK(car_passes(RestrictionSet{}, car));  // no fields: every car

  RestrictionSet eighties = decode_restriction_value("0003000004000007BC000007C5").set;
  for (uint32_t year : {1979u, 1980u, 1989u, 1990u}) {
    car.year = year;
    CHECK(car_passes(eighties, car) == (year >= 1980 && year <= 1989));
  }
  car.year = 1985;

  // Only a max: "pre-1980" style, no lower bound.
  RestrictionSet pre;
  pre.year_max = 1979;
  car.year = 1900;
  CHECK(car_passes(pre, car));
  car.year = 1980;
  CHECK(!car_passes(pre, car));
  car.year = 1985;

  RestrictionSet cheap = decode_restriction_value("00000000640000000000003A98").set;
  for (uint32_t value : {0u, 15000u, 15001u}) {
    car.value = value;
    CHECK(car_passes(cheap, car) == (value <= 15000));
  }
  RestrictionSet dear;
  dear.value_min = 100000;
  car.value = 99999;
  CHECK(!car_passes(dear, car));
  car.value = 100000;
  CHECK(car_passes(dear, car));
  car.value = 15000;

  RestrictionSet awd = decode_restriction_value("000000020403").set;
  CHECK(car_passes(awd, car));
  car.drive_type = kDriveFwd;
  CHECK(!car_passes(awd, car));
  car.drive_type = kDriveAwd;

  RestrictionSet make = decode_restriction_value("0000080004000D").set;
  CHECK(car_passes(make, car));
  car.make = 14;
  CHECK(!car_passes(make, car));
  car.make = 13;

  // Each ID field on its own.
  auto id_case = [&](auto RestrictionSet::*field, auto match, auto CarProperties::*prop, auto other) {
    RestrictionSet s;
    s.*field = match;
    CarProperties c = car;
    c.*prop = match;
    CHECK(car_passes(s, c));
    c.*prop = other;
    CHECK(!car_passes(s, c));
  };
  id_case(&RestrictionSet::body_family, uint8_t(4), &CarProperties::body_family, uint8_t(5));
  id_case(&RestrictionSet::region, uint8_t(2), &CarProperties::region, uint8_t(3));
  id_case(&RestrictionSet::country, uint8_t(7), &CarProperties::country, uint8_t(8));
  id_case(&RestrictionSet::model_family, uint16_t(300), &CarProperties::model_family, uint16_t(301));

  // The flag alone changes nothing.
  RestrictionSet flag;
  flag.flag34 = true;
  CHECK(car_passes(flag, car));
  RestrictionSet make_no_flag = make;
  make_no_flag.flag34 = false;
  car.make = 14;
  CHECK(car_passes(make, car) == car_passes(make_no_flag, car));
  car.make = 13;
}

void test_buckets() {
  EventRestrictions event;
  RestrictionDecode d;
  CHECK(set_restriction_bucket(event, 0, "000000020403", &d) == BucketError::kNone);
  CHECK(d.ok() && event.buckets[0].drive_type == kDriveAwd);
  CHECK(set_restriction_bucket(event, 3, "0000080004000D") == BucketError::kNone);
  CHECK(event.buckets[3].make == 13);
  // A rejected value stores "no restrictions".
  CHECK(set_restriction_bucket(event, 3, "000008000400", &d) == BucketError::kBadValue);
  CHECK(d.error == RestrictionError::kTruncated);
  CHECK(event.buckets[3].empty());
  // Out-of-range buckets leave the event unchanged.
  const EventRestrictions before = event;
  CHECK(set_restriction_bucket(event, 4, "0000080004000D") == BucketError::kOutOfRange);
  CHECK(set_restriction_bucket(event, -1, "0000080004000D") == BucketError::kOutOfRange);
  for (int i = 0; i < kRestrictionBuckets; ++i) CHECK(event.buckets[size_t(i)] == before.buckets[size_t(i)]);
}

}  // namespace

int main() {
  test_spec_valid_cases();
  test_spec_rejected_cases();
  test_other_errors();
  test_round_trip();
  test_predicate();
  test_buckets();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("all data tests passed\n");
  return 0;
}
