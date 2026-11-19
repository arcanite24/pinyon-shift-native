// event-restrictions-corpus: decodes every row of the EventRestrictions
// table in the player's game database and checks that each re-encodes to
// the same Value string (SPEC-EVENT-RESTRICTIONS, section "Tests").
//
//   event-restrictions-corpus --game-root <dir>
//
// <dir> is the extracted disc root containing media/ (or media itself);
// without --game-root the SHIFT_GAME_ROOT environment variable is used.
// The database is opened read-only and immutable, so nothing is written
// next to it. Only counts are printed, never values or event IDs.
// Exit code 0 when every row decodes and round-trips.

#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <utility>

#include <sqlite3.h>

#include "data/event_restrictions.h"

namespace fs = std::filesystem;
using namespace shift::data;

namespace {

std::string sqlite_uri(const fs::path& path) {
  std::string p = fs::absolute(path).generic_string();
  std::string out = "file:";
  if (!p.empty() && p[0] != '/') out += "///";  // drive-letter path
  else out += "//";
  for (char c : p) {
    if (c == '%' || c == '?' || c == '#' || c == ' ') {
      char buf[4];
      std::snprintf(buf, sizeof buf, "%%%02X", static_cast<unsigned char>(c));
      out += buf;
    } else {
      out += c;
    }
  }
  return out + "?mode=ro&immutable=1";
}

fs::path find_db(const fs::path& root) {
  for (const fs::path& candidate : {root / "media" / "db" / "gamedb.slt", root / "db" / "gamedb.slt"})
    if (fs::is_regular_file(candidate)) return candidate;
  return {};
}

std::string upper(std::string s) {
  for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  fs::path root;
  if (const char* env = std::getenv("SHIFT_GAME_ROOT")) root = env;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--game-root" && i + 1 < argc) {
      root = argv[++i];
    } else {
      std::fprintf(stderr, "usage: event-restrictions-corpus --game-root <dir>\n");
      return 2;
    }
  }
  if (root.empty()) {
    std::fprintf(stderr, "no game root: pass --game-root <dir> or set SHIFT_GAME_ROOT\n");
    return 2;
  }
  const fs::path db_path = find_db(root);
  if (db_path.empty()) {
    std::fprintf(stderr, "media/db/gamedb.slt not found under the game root\n");
    return 2;
  }

  sqlite3* db = nullptr;
  if (sqlite3_open_v2(sqlite_uri(db_path).c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr) !=
      SQLITE_OK) {
    std::fprintf(stderr, "cannot open the database: %s\n", db ? sqlite3_errmsg(db) : "out of memory");
    sqlite3_close(db);
    return 2;
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT EventID, BucketId, Value FROM EventRestrictions", -1, &stmt, nullptr) !=
      SQLITE_OK) {
    std::fprintf(stderr, "cannot read EventRestrictions: %s\n", sqlite3_errmsg(db));
    sqlite3_close(db);
    return 2;
  }

  uint64_t rows = 0, not_text = 0, bad_bucket = 0, duplicates = 0, decoded = 0, empty_sets = 0;
  uint64_t round_trip_exact = 0, round_trip_case_only = 0, round_trip_failed = 0;
  std::map<int64_t, uint64_t> buckets;
  std::map<RestrictionError, uint64_t> errors;
  std::array<uint64_t, 40> bits{};
  std::set<std::pair<int64_t, int64_t>> seen;
  std::set<int64_t> events;

  int step;
  while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
    ++rows;
    const int64_t event_id = sqlite3_column_int64(stmt, 0);
    const int64_t bucket = sqlite3_column_int64(stmt, 1);
    events.insert(event_id);
    ++buckets[bucket];
    if (bucket < 0 || bucket >= kRestrictionBuckets) ++bad_bucket;
    if (!seen.insert({event_id, bucket}).second) ++duplicates;
    if (sqlite3_column_type(stmt, 2) != SQLITE_TEXT) {
      ++not_text;
      continue;
    }
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    const std::string value(text, size_t(sqlite3_column_bytes(stmt, 2)));
    const RestrictionDecode d = decode_restriction_value(value);
    if (!d.ok()) {
      ++errors[d.error];
      continue;
    }
    ++decoded;
    if (d.set.empty()) ++empty_sets;
    const std::string mask = encode_restriction_value(d.set).substr(0, 10);
    for (int bit = 0; bit < 40; ++bit) {
      const int digit = std::stoi(mask.substr(size_t(2 * (bit / 8)), 2), nullptr, 16);
      if (digit >> (bit % 8) & 1) ++bits[size_t(bit)];
    }
    const std::string again = encode_restriction_value(d.set);
    if (again == value) ++round_trip_exact;
    else if (again == upper(value)) ++round_trip_case_only;
    else ++round_trip_failed;
  }
  const bool step_failed = step != SQLITE_DONE;
  if (step_failed) std::fprintf(stderr, "reading EventRestrictions failed: %s\n", sqlite3_errmsg(db));
  sqlite3_finalize(stmt);
  sqlite3_close(db);

  std::printf("EventRestrictions rows: %llu (events %zu)\n", (unsigned long long)rows, events.size());
  std::printf("buckets:");
  for (auto& [b, n] : buckets) std::printf(" %lld=%llu", (long long)b, (unsigned long long)n);
  std::printf("\nbucket outside 0-3: %llu, duplicate (event, bucket): %llu, non-text values: %llu\n",
              (unsigned long long)bad_bucket, (unsigned long long)duplicates, (unsigned long long)not_text);
  std::printf("decoded: %llu, empty sets: %llu\n", (unsigned long long)decoded, (unsigned long long)empty_sets);
  for (auto& [e, n] : errors) std::printf("rejected (%s): %llu\n", to_string(e), (unsigned long long)n);
  std::printf("re-encoded: %llu exact, %llu differ only in hex case, %llu differ\n",
              (unsigned long long)round_trip_exact, (unsigned long long)round_trip_case_only,
              (unsigned long long)round_trip_failed);
  std::printf("rows per bit:");
  for (int bit = 0; bit < 40; ++bit)
    if (bits[size_t(bit)]) std::printf(" %d=%llu", bit, (unsigned long long)bits[size_t(bit)]);
  std::printf("\n");

  const bool ok = !step_failed && rows > 0 && not_text == 0 && bad_bucket == 0 && duplicates == 0 &&
                  errors.empty() && round_trip_case_only == 0 && round_trip_failed == 0;
  std::printf("%s\n", ok ? "corpus OK" : "corpus FAILED");
  return ok ? 0 : 1;
}
