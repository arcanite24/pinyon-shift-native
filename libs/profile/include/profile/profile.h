// The player's Forza Horizon save as the engine imports it (FORMAT-PROFILE).
//
// The import surface is the plaintext export of the player's own profile
// (FORMAT-PROFILE, Unknowns: the engine carries no key material): a folder
// with `ForzaProfile.body` (the profile body), `PlayerDatabase.sqlite` (the
// inflated database) and, optionally, `content-packages.txt` and
// `content-disabled.txt` (the installed DLC package folders, one a line).
//
// The body is a self-describing field tree (big-endian: a u32 count, then
// per field a u32 name length, the name, u32 0x20, u32 0, a type byte and
// the value; a struct is a u32 count and that many fields) followed by
// blocks of class-serialised records (u32 version, u32 count; per record
// the name, the class name, a u32 version and a sized payload; a parent's
// payload is a u32 child count and its children in the same layout). Every
// record is kept as bytes, so unknown classes and versions survive.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace profile {

struct Field;

struct Value {
  enum Type : uint8_t { kBool = 0x00, kU8 = 0x01, kU32 = 0x03, kU64 = 0x04, kI32 = 0x07, kFloat = 0x09, kStruct = 0x0F };
  Type type = kU32;
  uint64_t bits = 0;          // the raw value (u8/u32/u64/i32/float bits)
  std::vector<Field> fields;  // a struct's
  int64_t as_int() const;     // signed for i32, else unsigned
  double as_double() const;   // a float's value, else as_int
};

struct Field {
  std::string name;
  Value value;
};

struct Record {
  std::string name, class_name;
  uint32_t version = 0;
  std::vector<uint8_t> payload;  // a parent's whole payload too
  std::vector<Record> children;  // CHorizonLocationState's
};

struct Block {
  uint32_t version = 0;
  std::vector<Record> records;
};

struct Body {
  std::vector<Field> fields;
  std::vector<Block> blocks;
  std::vector<uint8_t> trailing_bytes;  // trailing slack / 0xBB padding preserved verbatim
};

// Parses a profile body; false with `error` on a bad type byte or a field
// or record that runs past the end. The blocks stop at fewer than 8 bytes
// left or a 0xBB padding word (any remaining bytes are preserved in `trailing_bytes`).
bool parse_body(std::span<const uint8_t> bytes, Body& out, std::string* error);
// Serializes a parsed or constructed profile body back to big-endian bytes,
// preserving fields, record blocks, CHorizonLocationState children and trailing bytes.
std::vector<uint8_t> serialize_body(const Body& body);
// A field by path ("Main/Credits"), or null.
const Field* find(const std::vector<Field>& fields, std::string_view path);
Field* find_mut(std::vector<Field>& fields, std::string_view path);
// Sets a scalar field by path, preserving its existing type if present or
// creating missing structs/fields with `default_type`.
void set_field_int(std::vector<Field>& fields, std::string_view path, int64_t value,
                   Value::Type default_type = Value::kI32);
void set_field_double(std::vector<Field>& fields, std::string_view path, double value,
                      Value::Type default_type = Value::kFloat);
void set_field_bool(std::vector<Field>& fields, std::string_view path, bool value);
// A struct's field count, or 0.
size_t field_count(const std::vector<Field>& fields, std::string_view path);

struct Sign {
  std::string name;
  bool revealed = false, found = false;
};
struct Barn {
  std::string name;
  bool revealed = false, discovered = false, collected = false;
  float rumour_at = 0, found_at = 0;  // m driven (the record has found first)
  uint32_t status = 3;  // 2: found and delivered (SPEC-FREEROAM-COLLECTIBLES)
};
struct CarHistory {
  double distance_driven = 0.0;  // DistanceDriven (m)
  double time_driven = 0.0;      // TimeDriven (s)
  int64_t total_winnings = 0;
  int64_t total_repairs = 0;
  int64_t num_races = 0;
  int64_t num_victories = 0;
  int64_t num_podiums = 0;
};
struct GarageCar {
  int64_t id = 0, car_id = 0;
  double performance_index = 0;  // raw, (0, 1]
  int64_t class_id = 0, color_index = 0;
  CarHistory history;
  // Every integer, float, and text column of the row by name (the part slots,
  // the tune, livery filename, GUID, and history), kept as read.
  std::map<std::string, int64_t> columns;
  std::map<std::string, double> reals;       // the float columns (CurbWeight, Tuning_*, ratings)
  std::map<std::string, std::string> texts;  // the text columns (LiveryFileName, Guid, etc.)
};
struct PurchasedPart {
  int64_t garage_id = 0, part_enum = 0, part_id = 0, manufacturer_id = 0, price_paid = 0;
};
struct ClubCar {
  int64_t car_id = 0;
  int challenges = 0, done = 0;  // flag A set
  std::vector<std::pair<uint32_t, bool>> keys;    // each challenge's key and flag A, in file order
  std::vector<bool> flags_b;                      // flag B (online confirmation), parallel to `keys`
  std::vector<std::vector<int>> progress;         // optional per-challenge save_progress counters (8-byte stride)
  std::vector<std::vector<uint8_t>> raw_blobs;    // raw save_progress blob bytes, parallel to `keys`
};

// One `CSponsorshipChallengeState` child of `SponsorshipState` (v1, 8 bytes:
// u32 tiers claimed, u32 count). The tiers word equals the number of the
// SponsorshipChallenges.xml ranks whose `number` the count reaches on all 28
// skill children of both reference saves; on `Fame` it is the Fame-sponsor
// tiers claimed and the count is 0.
struct SponsorshipChallenge {
  std::string id;  // the SponsorshipChallenges.xml id ("Air", "Fame", ...)
  uint32_t tiers_claimed = 0, count = 0;
};

// `SponsorshipState` (`CHorizonSponsorshipState` v2): a 20-byte header (u32
// popularity points, a 4-byte word, f32, u32 2, u32 child count) and the
// child records. Only the popularity word and the 8-byte children are
// interpreted; the other header words are kept as read.
struct SponsorshipState {
  uint32_t popularity_points = 0;
  std::vector<SponsorshipChallenge> challenges;
  const SponsorshipChallenge* find(std::string_view id) const;
};

// Decodes a `SponsorshipState` payload; false when it does not parse.
bool decode_sponsorship_state(std::span<const uint8_t> payload, SponsorshipState& out);
// The one place the save's popularity is read (SPEC_QUESTIONS Q60, probable):
// the first header word of `SponsorshipState`, cumulative Fame.xml points.
// 0 when the payload does not parse.
uint32_t popularity_points_from_sponsorship_state(std::span<const uint8_t> payload);
// `payload` with the popularity word and the named children's two words
// replaced; children absent from `payload` are appended (count updated),
// every other byte is kept. An empty `payload` gets a fresh record with the
// header layout of the reference saves (word 0, f32 0, 2) and the 15
// SponsorshipChallenges.xml ids.
std::vector<uint8_t> encode_sponsorship_state(std::span<const uint8_t> payload, const SponsorshipState& st);

// Everything M6 reads from a profile, with the parsed body kept whole.
struct Imported {
  Body body;
  uint32_t version_flags = 0;  // 0 base, 2 v4 (missing: base)
  size_t main_fields = 0;
  // Main/*: the career.
  // `level` is Main/Level, the zero-based PlayerLevelDef.Id kept as saved
  // (the engine's driver level is that plus one); `last_rewarded_level` is
  // Main/LastRewardedLevel. The profile has no driver XP field.
  int64_t credits = 0, xp = 0, wristband_level = 0, level = 0, career_car = 0, upgrade_discount = 0;
  int64_t last_rewarded_level = 0;
  int64_t total_winnings = 0, num_races = 0, num_victories = 0, num_podiums = 0;
  // Popularity points and the sponsors (`SponsorshipState`, see above);
  // `fame_tiers_claimed` is the `Fame` child's tiers word.
  double popularity = 0;
  bool has_sponsorship = false;
  std::vector<SponsorshipChallenge> sponsors;  // every child, Fame included
  int64_t fame_tiers_claimed = 0;
  // Main/MaxSkillsScore and Main/MaxSkillsCarOrdinal (a Data_Car id).
  int64_t max_skills_score = 0, max_skills_car = 0;
  // Colorado's `CFirstTimeCareerState` (5 bytes: a byte, u32 stage; stage 1
  // the opening Viper race, 2 the Corrado drive to the festival; 8 on both
  // finished reference saves).
  bool has_first_time_career = false;
  uint8_t first_time_career_flag = 0;
  uint32_t first_time_career_stage = 0;
  double top_speed = 0;        // m/s
  double distance_driven = 0;  // TotalDistanceDriven, m
  double time_festival = 0, time_street = 0, time_exhibition = 0;  // TimeIn*Races, s
  bool rally_fields = false;
  int64_t num_rallies = 0, num_rally_stages = 0, num_rally_victories = 0, rally_winnings = 0;
  int64_t rally_xp = 0, rally_level = 0;  // RallyXP, RallyWristbandLevel
  std::vector<Sign> signs;
  std::vector<Barn> barns;
  std::vector<ClubCar> club;
  std::vector<uint8_t> club_trailing;  // preserved trailing bytes of CCarChallengeState
  // CRoadDiscovery (Colorado): node count + ceil(node_count/32) uint32 words.
  uint32_t road_discovery_nodes = 0;
  std::vector<uint32_t> road_discovery_words;
  // CGasStationState, CMissionState & speed activity PBs (Colorado).
  std::vector<std::string> outposts_discovered;
  std::vector<std::string> pr_stunts_completed;
  std::vector<std::pair<std::string, double>> speed_traps;
  std::vector<std::pair<std::string, double>> speed_zones;
  bool satnav_on = true;
  bool fast_travel_anywhere = false;  // Main/FastTravelAnywhere (v4)
  bool has_codriver = false;
  bool codriver_on = true;            // Main/CoDriver (v4)
  // PlayerDatabase.
  std::vector<GarageCar> garage;
  std::vector<PurchasedPart> parts;
  std::vector<std::pair<int64_t, int>> event_results, race_results;  // id, best place
  std::vector<int64_t> event_unlocks, hub_unlocks;
  int64_t last_used_garage_id = 0;
  // Content packages (DLC ownership: folders with a header, not disabled).
  std::vector<std::string> packages, disabled_packages;

  size_t signs_found() const;
  size_t barns_found() const;
  // The selected garage row: CareerCar, or the lowest Id when no row has it.
  const GarageCar* selected_car() const;
};

// Reads an export folder; false with `error` on a missing body or database
// (FORMAT-PROFILE: refuse) or a body that does not parse.
bool import_profile(const std::filesystem::path& folder, Imported& out, std::string* error);

// Writes a plaintext ForzaProfile export folder (`ForzaProfile.body`,
// `PlayerDatabase.sqlite`, `export.json`, and optional package lists) atomically.
// When `template_db` names an existing SQLite database, its schema and
// non-career tables are preserved while career tables are updated.
bool export_forza_profile(const Imported& p, const std::filesystem::path& out_folder, std::string* error,
                          const std::filesystem::path& template_db = {});

// The FORMAT-PROFILE Tests counts, in profile-export.py summary's keys.
std::map<std::string, int64_t> summary(const Imported& p);

}  // namespace profile
